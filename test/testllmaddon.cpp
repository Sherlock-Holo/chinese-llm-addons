/*
 * SPDX-FileCopyrightText: 2026-2026
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
// Each scenario uses a real fcitx instance, pinyin engine and HTTP worker.
// smoke.sh supplies build-tree metadata, an isolated config/data home and mock.
#include "chinese-llm.h"
#include "testfrontend_public.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <curl/curl.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/log.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/surroundingtext.h>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fcitx;
using fcitx::chinesellm::LlmState;

namespace {
constexpr char kKeys[] = "tamenzhengzaikaihui";
constexpr char kModelWord[] = "她们正在开会";
constexpr char kOtherWord[] = "它们正在开会";
using Clock = std::chrono::steady_clock;

bool hasCandidate(InputContext *ic, const std::string &word) {
    const auto list = ic->inputPanel().candidateList();
    auto *bulk = list ? list->toBulk() : nullptr;
    if (!bulk) {
        return false;
    }
    for (int i = 0; i < bulk->totalSize(); ++i) {
        if (bulk->candidateFromAll(i).text().toString() == word) {
            return true;
        }
    }
    return false;
}

void selectCandidate(InputContext *ic, const std::string &word) {
    auto list = ic->inputPanel().candidateList();
    auto *bulk = list ? list->toBulk() : nullptr;
    if (bulk) {
        for (int i = 0; i < bulk->totalSize(); ++i) {
            if (bulk->candidateFromAll(i).text().toString() == word) {
                bulk->candidateFromAll(i).select(ic);
                return;
            }
        }
    }
    throw std::runtime_error("candidate not found: " + word);
}

size_t mockRequestCount(const std::string &endpoint) {
    const auto path = endpoint.find('/', endpoint.find("://") + 3);
    const auto url = endpoint.substr(0, path) + "/stats";
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                             curl_easy_cleanup);
    if (!curl) {
        throw std::runtime_error("failed to initialize stats request");
    }
    std::string body;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 500L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(
        curl.get(), CURLOPT_WRITEFUNCTION,
        +[](char *data, size_t size, size_t count, void *context) {
            const auto bytes = size * count;
            static_cast<std::string *>(context)->append(data, bytes);
            return bytes;
        });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    if (curl_easy_perform(curl.get()) != CURLE_OK) {
        throw std::runtime_error("mock stats unavailable");
    }
    return nlohmann::json::parse(body).at("count").get<size_t>();
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace

int main(int argc, char **argv) {
    std::string endpoint = "http://127.0.0.1:8899/v1/chat/completions";
    std::string scenario = "live";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.starts_with("--scenario=")) {
            scenario = arg.substr(11);
        } else if (arg.starts_with("http://") || arg.starts_with("https://")) {
            endpoint = arg;
        } else {
            std::cerr << "unknown argument: " << arg << '\n';
            return EXIT_FAILURE;
        }
    }
    const std::vector<std::string> scenarios{"live",
                                             "debounce",
                                             "disabled",
                                             "dead",
                                             "preview",
                                             "reset",
                                             "partial",
                                             "toggle",
                                             "sensitive",
                                             "toggle-inflight",
                                             "sensitive-inflight",
                                             "context",
                                             "cache",
                                             "cooldown",
                                             "config",
                                             "filtered"};
    if (std::ranges::find(scenarios, scenario) == scenarios.end()) {
        std::cerr << "unknown scenario: " << scenario << '\n';
        return EXIT_FAILURE;
    }
    Log::setLogRule("default=3,chinese-llm=4");
    char arg0[] = "testllmaddon";
    char arg1[] = "--disable=all";
    char arg2[] = "--enable=testfrontend,punctuation,pinyin,chinese-llm";
    char *instanceArgs[] = {arg0, arg1, arg2};
    Instance instance(3, instanceArgs);
    instance.addonManager().registerDefaultLoader(nullptr);

    bool completed = false;
    int exitCode = EXIT_FAILURE;
    int stage = 0;
    uint64_t generation = 0;
    InputContext *ic = nullptr;
    AddonInstance *frontend = nullptr;
    AddonInstance *llm = nullptr;
    AddonInstance *pinyin = nullptr;
    ICUUID uuid{};
    Clock::time_point start;
    Clock::time_point stageStart;
    std::unique_ptr<EventSourceTime> poll;

    auto finish = [&](bool success, const std::string &message) {
        completed = true;
        exitCode = success ? EXIT_SUCCESS : EXIT_FAILURE;
        if (success) {
            std::cout << "all addon tests passed: " << scenario << '\n';
        } else {
            std::cerr << "FAIL [" << scenario << "]: " << message << '\n';
        }
        if (ic) {
            frontend->call<ITestFrontend::destroyInputContext>(uuid);
            ic = nullptr;
        }
        instance.exit();
    };
    auto type = [&](const std::string &keys) {
        for (char c : keys) {
            frontend->call<ITestFrontend::sendKeyEvent>(
                uuid, Key(std::string(1, c)), false);
        }
    };
    auto state = [&]() { return ic->propertyAs<LlmState>("chinese-llm"); };
    auto checkCommit = [&](const std::string &word) {
        frontend->call<ITestFrontend::pushCommitExpectation>(word);
        selectCandidate(ic, word);
        require(ic->inputPanel().clientPreedit().empty() &&
                    ic->inputPanel().preedit().empty(),
                "both preedit fields must clear after commit");
        require(state()->pinyin.empty(), "LLM composition was not reset");
        type("shui");
        auto raw = ic->inputPanel().preedit().toString();
        if (raw.empty()) {
            raw = ic->inputPanel().clientPreedit().toString();
        }
        require(raw == "shui",
                "pinyin engine retained old composition: " + raw);
        ic->reset();
    };

    instance.eventDispatcher().schedule([&]() {
        try {
            frontend = instance.addonManager().addon("testfrontend", true);
            pinyin = instance.addonManager().addon("pinyin", true);
            llm = instance.addonManager().addon("chinese-llm", true);
            require(frontend && pinyin && llm,
                    "testfrontend, pinyin and chinese-llm must all load");
            auto group = instance.inputMethodManager().currentGroup();
            group.inputMethodList().clear();
            // The installed keyboard engine may be statically linked into the
            // fcitx executable rather than available to an external test
            // binary.
            group.inputMethodList().emplace_back("pinyin");
            group.setDefaultInputMethod("pinyin");
            instance.inputMethodManager().setGroup(std::move(group));

            RawConfig config;
            config.setValueByPath("Endpoint", endpoint);
            config.setValueByPath("Enabled",
                                  scenario == "disabled" ? "False" : "True");
            config.setValueByPath("Model", "mock");
            config.setValueByPath("ApiKey", "");
            config.setValueByPath("DebounceMs", "250");
            config.setValueByPath("TimeoutSeconds", "1");
            config.setValueByPath("PromptTemplate", "{context}\n{pinyin}");
            config.setValueByPath("ContextLength", "2");
            config.setValueByPath(
                "CacheSize",
                scenario == "cache" || scenario == "config" ? "256" : "0");
            config.setValueByPath("MaxConsecutiveErrors", "2");
            llm->setConfig(config);
            if (scenario == "preview") {
                RawConfig preview;
                preview.setValueByPath("PreeditMode", "Commit preview");
                pinyin->setConfig(preview);
            }
            uuid = frontend->call<ITestFrontend::createInputContext>(
                "testllmaddon");
            ic = instance.inputContextManager().findByUUID(uuid);
            require(ic, "failed to create input context");
            ic->setCapabilityFlags(
                scenario == "preview" ? CapabilityFlags(CapabilityFlag::Preedit)
                                      : CapabilityFlags{});
            if (scenario == "sensitive") {
                ic->setCapabilityFlags(CapabilityFlag::PasswordOrSensitive);
            }
            ic->focusIn();
            instance.setCurrentInputMethod(ic, "pinyin", true);
            require(scenario == "sensitive" ||
                        instance.inputMethod(ic) == "pinyin",
                    "pinyin is not active");
            if (scenario == "context" || scenario == "cache") {
                ic->surroundingText().setText("甲乙丙丁戊", 3, 3);
                ic->updateSurroundingText();
            }
            start = stageStart = Clock::now();
            type(kKeys);
            generation = state()->generation;
            if (scenario == "toggle") {
                require(state()->debounce && state()->debounce->isEnabled(),
                        "expected pending debounce before toggle");
                frontend->call<ITestFrontend::sendKeyEvent>(
                    uuid, Key("Control+Alt+Shift+L"), false);
                require(!state()->debounce, "toggle left debounce timer alive");
            }
            poll = instance.eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 20000, 0,
                [&](EventSourceTime *timer, uint64_t) {
                    try {
                        const auto elapsed = Clock::now() - start;
                        const auto stageElapsed = Clock::now() - stageStart;
                        require(elapsed < std::chrono::seconds(8),
                                "scenario deadline exceeded");
                        const bool appeared = hasCandidate(ic, kModelWord);
                        if (scenario == "disabled" || scenario == "dead" ||
                            scenario == "sensitive" || scenario == "toggle") {
                            require(!appeared, "unexpected model candidate");
                            if (elapsed > std::chrono::milliseconds(1800)) {
                                require(!state()->debounce ||
                                            !state()->debounce->isEnabled(),
                                        "unexpected pending debounce");
                                finish(true, "");
                            }
                        } else if (scenario == "toggle-inflight" ||
                                   scenario == "sensitive-inflight") {
                            if (stage == 0 && mockRequestCount(endpoint) >= 1) {
                                require(state()->requestSeq != 0,
                                        "request did not start");
                                if (scenario == "toggle-inflight") {
                                    frontend->call<ITestFrontend::sendKeyEvent>(
                                        uuid, Key("Control+Alt+Shift+L"),
                                        false);
                                } else {
                                    ic->setCapabilityFlags(
                                        CapabilityFlag::PasswordOrSensitive);
                                }
                                require(
                                    state()->generation > generation &&
                                        state()->pinyin.empty(),
                                    "in-flight request was not invalidated");
                                stage = 1;
                            }
                            require(!appeared,
                                    "cancelled in-flight result was inserted");
                            if (stage == 1 &&
                                elapsed > std::chrono::milliseconds(1800)) {
                                finish(true, "");
                            }
                        } else if (scenario == "partial") {
                            if (stage == 0 && mockRequestCount(endpoint) >= 1) {
                                require(state()->requestSeq != 0,
                                        "request did not start");
                                selectCandidate(ic, "他");
                                require(state()->generation > generation,
                                        "partial selection did not invalidate "
                                        "reply");
                                stage = 1;
                            }
                            require(!appeared,
                                    "stale full-sentence result inserted after "
                                    "partial selection");
                            if (stage == 1 &&
                                elapsed > std::chrono::milliseconds(1800)) {
                                finish(true, "");
                            }
                        } else if (scenario == "reset") {
                            if (stage == 0 && mockRequestCount(endpoint) >= 1) {
                                require(state()->requestSeq != 0,
                                        "request did not start");
                                ic->reset();
                                require(
                                    state()->pinyin.empty() &&
                                        state()->generation > generation,
                                    "reset did not clear composition/version");
                                type(kKeys);
                                stage = 1;
                            } else if (stage == 1 && appeared) {
                                checkCommit(kModelWord);
                                finish(true, "");
                            }
                        } else if (scenario == "context" ||
                                   scenario == "cache") {
                            if (stage == 0 && appeared) {
                                require(state()->context == "乙丙",
                                        "wrong context before middle cursor");
                                require(!hasCandidate(ic, kOtherWord),
                                        "wrong contextual result");
                                ic->reset();
                                ic->surroundingText().setCursor(5, 5);
                                ic->updateSurroundingText();
                                type(kKeys);
                                stage = 1;
                            } else if (stage == 1 &&
                                       hasCandidate(ic, kOtherWord)) {
                                require(state()->context == "丁戊",
                                        "wrong context at end cursor");
                                require(!appeared,
                                        "reused old-context cached result");
                                if (scenario == "cache") {
                                    ic->reset();
                                    ic->surroundingText().setCursor(3, 3);
                                    ic->updateSurroundingText();
                                    type(kKeys);
                                    require(hasCandidate(ic, kModelWord),
                                            "expected synchronous cache hit");
                                }
                                finish(true, "");
                            }
                        } else if (scenario == "cooldown") {
                            require(!appeared,
                                    "error endpoint produced a candidate");
                            if (stage < 2 &&
                                stageElapsed > std::chrono::milliseconds(900)) {
                                ic->reset();
                                type(stage == 0 ? "shui" : kKeys);
                                stage++;
                                stageStart = Clock::now();
                                if (stage == 2) {
                                    require(state()->pinyin.empty() &&
                                                !state()->debounce,
                                            "second error did not activate "
                                            "cooldown");
                                }
                            } else if (stage == 2 &&
                                       stageElapsed >
                                           std::chrono::milliseconds(900)) {
                                finish(true, "");
                            }
                        } else if (scenario == "filtered" && appeared) {
                            auto list = ic->inputPanel().candidateList();
                            auto *common =
                                dynamic_cast<CommonCandidateList *>(list.get());
                            require(common, "expected common candidate list");
                            common->setFilter([](const CandidateWord &word) {
                                return word.text().toString() == kModelWord;
                            });
                            const auto filteredSize = common->totalSize();
                            const auto rawSize = common->originSize();
                            const auto oldGeneration = state()->generation;
                            require(!common->toModifiable(),
                                    "filter did not make view immutable");
                            frontend->call<ITestFrontend::sendKeyEvent>(
                                uuid, Key("Control+Alt+Shift+L"), false);
                            require(state()->generation > oldGeneration,
                                    "toggle failed to invalidate filtered "
                                    "candidate");
                            require(common->totalSize() == filteredSize &&
                                        common->originSize() == rawSize,
                                    "invalidation corrupted engine "
                                    "filter/dictionary candidates");
                            const auto rawPreedit =
                                ic->inputPanel().preedit().toString();
                            selectCandidate(ic, kModelWord);
                            require(ic->inputPanel().preedit().toString() ==
                                            rawPreedit &&
                                        !rawPreedit.empty(),
                                    "stale filtered candidate was committed");
                            common->clearFilter();
                            selectCandidate(ic, kModelWord);
                            require(ic->inputPanel().preedit().toString() ==
                                        rawPreedit,
                                    "stale candidate was committed after "
                                    "filter cleared");
                            finish(true, "");
                        } else if (scenario == "config") {
                            if (stage == 0 && appeared) {
                                const auto oldGeneration = state()->generation;
                                RawConfig change;
                                change.setValueByPath("Model",
                                                      "different-model");
                                llm->setConfig(change);
                                require(state()->generation > oldGeneration &&
                                            !hasCandidate(ic, kModelWord),
                                        "config left stale model "
                                        "candidate/version");
                                ic->updateUserInterface(
                                    UserInterfaceComponent::InputPanel);
                                require(state()->debounce &&
                                            state()->debounce->isEnabled(),
                                        "new model reused old cache");
                                stage = 1;
                            } else if (stage == 1 && appeared) {
                                finish(true, "");
                            }
                        } else if (appeared) {
                            checkCommit(kModelWord);
                            finish(true, "");
                        }
                    } catch (const std::exception &error) {
                        finish(false, error.what());
                    }
                    if (!completed) {
                        timer->setTime(now(CLOCK_MONOTONIC) + 20000);
                        timer->setOneShot();
                    }
                    return true;
                });
            poll->setOneShot();
        } catch (const std::exception &error) {
            finish(false, error.what());
        }
    });
    const auto instanceExitCode = instance.exec();
    return completed && instanceExitCode == 0 ? exitCode : EXIT_FAILURE;
}
