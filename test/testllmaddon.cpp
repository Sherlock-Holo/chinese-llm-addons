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
#include <fcitx-config/dbushelper.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/dbus/variant.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/log.h>
#include <fcitx/action.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/globalconfig.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/statusarea.h>
#include <fcitx/surroundingtext.h>
#include <fcitx/userinterfacemanager.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace fcitx;
using fcitx::chinesellm::LlmState;

namespace {
constexpr char kLlmInputMethod[] = "llm-pinyin";
constexpr char kKeys[] = "tamenzhengzaikaihui";
constexpr char kModelWord[] = "她们正在开会";
constexpr char kOtherWord[] = "它们正在开会";
constexpr char kContextBefore[] = "safe-context-before";
constexpr char kContextAfter[] = "safe-context-after";
const std::map<std::string, std::pair<std::string, std::string>> payloadScenarios{
    {"mixed", {"bang'wo'push'yi'xia", "帮我push一下"}},
    {"mixed-context-changed", {"bangwocommityixia", "帮我commit一下"}}};
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

std::string configSnapshot(const Configuration *config) {
    if (!config) {
        throw std::runtime_error("input method configuration is missing");
    }
    RawConfig saved;
    config->save(saved);
    std::ostringstream output;
    writeAsIni(saved, output);
    return output.str();
}

std::string preeditText(InputContext *ic) {
    auto raw = ic->inputPanel().preedit().toString();
    return raw.empty() ? ic->inputPanel().clientPreedit().toString() : raw;
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
std::string normalizedTypedKeys(const std::string &text) {
    std::string result;
    for (char c : text) {
        if (c != ' ' && c != '\'') {
            result.push_back(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        }
    }
    return result;
}

void dumpComposition(InputContext *ic, const std::string &phase,
                     const std::string &keys, bool accepted) {
    const auto *composition = ic->propertyAs<LlmState>("chinese-llm");
    const auto list = ic->inputPanel().candidateList();
    const auto *bulk = list ? list->toBulk() : nullptr;
    const auto *common = dynamic_cast<const CommonCandidateList *>(list.get());
    nlohmann::json diagnostic{
        {"phase", phase},
        {"typed_keys", keys},
        {"key_accepted", accepted},
        {"focused", ic->hasFocus()},
        {"preedit", ic->inputPanel().preedit().toString()},
        {"client_preedit", ic->inputPanel().clientPreedit().toString()},
        {"state_pinyin", composition->pinyin},
        {"generation", composition->generation},
        {"request_seq", composition->requestSeq},
        {"debounce_present", static_cast<bool>(composition->debounce)},
        {"debounce_enabled", composition->debounce &&
                                 composition->debounce->isEnabled()},
        {"native_list_present", static_cast<bool>(list)},
        {"native_list_empty", !list || list->empty()},
        {"native_list_modifiable", list && list->toModifiable()},
        {"candidate_count", bulk ? bulk->totalSize() : -1},
        {"origin_count", common ? common->originSize() : -1},
        {"model_candidates", composition->candidates}};
    std::vector<std::string> firstWords;
    if (bulk) {
        for (int i = 0; i < std::min(bulk->totalSize(), 3); ++i) {
            firstWords.push_back(bulk->candidateFromAll(i).text().toString());
        }
    }
    diagnostic["first_candidates"] = firstWords;
    std::cout << "composition-diagnostic: " << diagnostic.dump() << std::endl;
}

std::string readTestData(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream),
            "missing isolated data " + path.string());
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

void checkPrivateSnapshot(const std::string &snapshot) {
    require(!snapshot.empty(), "history snapshot path is empty after viewing");
    const std::filesystem::path path(snapshot);
    struct stat fileStat {}, directoryStat {};
    require(::lstat(path.c_str(), &fileStat) == 0 &&
                ::lstat(path.parent_path().c_str(), &directoryStat) == 0 &&
                S_ISREG(fileStat.st_mode) && S_ISDIR(directoryStat.st_mode) &&
                (fileStat.st_mode & 0777) == 0600 &&
                (directoryStat.st_mode & 0777) == 0700 &&
                fileStat.st_uid == ::geteuid() &&
                directoryStat.st_uid == ::geteuid(),
            "history snapshot must be owned 0600 file inside owned 0700 dir");
}

std::filesystem::path openerLogPath() {
    const char *path = std::getenv("FCITX_TEST_HISTORY_OPENER_LOG");
    require(path && *path, "history menu test requires isolated xdg-open stub");
    return path;
}

bool openerRecorded() {
    const auto path = openerLogPath();
    return std::filesystem::exists(path) &&
           nlohmann::json::parse(readTestData(path), nullptr, false).is_object();
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
    const std::vector<std::string> scenarios{
        "live",  "mixed",  "mixed-context-changed", "cache",   "sensitive",
        "dead",  "toggle", "history-menu",          "stats-overlap", "llm-alone"};
    if (std::ranges::find(scenarios, scenario) == scenarios.end()) {
        std::cerr << "unknown scenario: " << scenario << '\n';
        return EXIT_FAILURE;
    }
    const auto payloadScenario = payloadScenarios.find(scenario);
    const bool payload = payloadScenario != payloadScenarios.end();
    const bool contiguousPayload = scenario == "mixed-context-changed";
    Log::setLogRule("default=3,chinese-llm=4");
    char arg0[] = "testllmaddon";
    char arg1[] = "--disable=all";
    const bool alone = scenario == "llm-alone";
    const std::string privateHelpers =
        ",chinese-llm-punctuation,chinese-llm-fullwidth,chinese-llm-chttrans,"
        "chinese-llm-pinyinhelper";
    std::string enabledAddons =
        (alone ? "--enable=testfrontend,chinese-llm"
               : "--enable=testfrontend,punctuation,pinyin,fullwidth,chttrans,"
                 "chinese-llm") +
        privateHelpers;
    char *instanceArgs[] = {arg0, arg1, enabledAddons.data()};
    Instance instance(3, instanceArgs);
    instance.addonManager().registerDefaultLoader(nullptr);

    bool completed = false;
    int exitCode = EXIT_FAILURE;
    int stage = 0;
    uint64_t generation = 0;
    InputContext *ic = nullptr;
    AddonInstance *frontend = nullptr;
    AddonInstance *llm = nullptr;
    fcitx::chinesellm::ChineseLlm *llmAddon = nullptr;
    AddonInstance *pinyin = nullptr;
    int statisticsStatusUpdates = 0;
    std::unique_ptr<HandlerTableEntry<EventHandler>> statisticsWatcher;
    ICUUID uuid{};
    Clock::time_point start;
    Clock::time_point stageStart;
    std::unique_ptr<EventSourceTime> poll;
    std::string officialConfigBefore;
    std::string shuangpinConfigBefore;
    std::string viewedSnapshot;
    std::string snapshotBeforeAdoption;
    uint64_t contextChangedGeneration = 0;
    const InputMethodEntry *officialEntry = nullptr;
    const InputMethodEntry *shuangpinEntry = nullptr;
    InputMethodEngine *officialEngine = nullptr;
    InputMethodEngine *llmEngine = nullptr;
    const std::string initialInputMethod = kLlmInputMethod;

    auto statisticsActions = [&]() {
        return std::vector<Action *>{
            instance.userInterfaceManager().lookupAction(
                "chinese-llm-stat-requests"),
            instance.userInterfaceManager().lookupAction(
                "chinese-llm-stat-overlap"),
            instance.userInterfaceManager().lookupAction(
                "chinese-llm-stat-rate")};
    };
    auto checkStatistics = [&](uint64_t sent, uint64_t compared,
                               uint64_t overlapping) {
        require(llmAddon, "LLM statistics public API is unavailable");
        const auto actual = llmAddon->requestStatistics();
        require(actual.sent == sent && actual.compared == compared &&
                    actual.overlapping == overlapping,
                "request statistics mismatch: got " +
                    std::to_string(actual.sent) + "/" +
                    std::to_string(actual.compared) + "/" +
                    std::to_string(actual.overlapping) + ", expected " +
                    std::to_string(sent) + "/" + std::to_string(compared) +
                    "/" + std::to_string(overlapping));
        std::string rate = "—";
        if (compared) {
            std::ostringstream formatted;
            formatted.imbue(std::locale::classic());
            formatted << std::fixed << std::setprecision(1)
                      << 100.0 * overlapping / compared << '%';
            rate = formatted.str();
        }
        const std::vector<std::pair<std::string, std::string>> labels{
            {"LLM requests " + std::to_string(sent) + " (valid " +
                 std::to_string(compared) + ")",
             "LLM 请求 " + std::to_string(sent) + "（有效 " +
                 std::to_string(compared) + "）"},
            {"LLM overlapping requests " + std::to_string(overlapping),
             "LLM 重叠请求 " + std::to_string(overlapping)},
            {"LLM overlap rate " + rate, "LLM 重叠率 " + rate}};
        const auto actions = statisticsActions();
        for (size_t i = 0; i < actions.size(); ++i) {
            auto *action = actions[i];
            require(action && dynamic_cast<SimpleAction *>(action) &&
                        !action->isCheckable() && action->hotkey().empty(),
                    "statistics item is missing/not a read-only SimpleAction");
            const auto label = action->shortText(ic);
            require(label == labels[i].first || label == labels[i].second,
                    "statistics status label mismatch: " + label);
            require(label.find(':') == std::string::npos &&
                        action->longText(ic).find(':') == std::string::npos,
                    "statistics item contains Kimpanel protocol delimiter");
            if (ic) {
                bool visible = instance.inputMethod(ic) == kLlmInputMethod;
                // This standalone fixture cannot load Fcitx's statically
                // linked keyboard addon. Password redirection then has no
                // engine, so core activation returns before clearing the old
                // status group. In that specific fallback, statistics must
                // follow the retained private toggle, not an official IM.
                if (ic->capabilityFlags().testAny(
                        CapabilityFlag::PasswordOrSensitive) &&
                    !instance.inputMethodEngine(ic)) {
                    auto *toggle = instance.userInterfaceManager().lookupAction(
                        "chinese-llm-toggle");
                    require(toggle,
                            "missing private toggle in password fixture");
                    visible = ic->statusArea().isChild(toggle);
                }
                require(ic->statusArea().isChild(action) == visible,
                        "statistics menu visibility leaked/lost across IMs");
            }
        }
        for (const auto *id : {"chinese-llm-history",
                               "chinese-llm-history-clear"}) {
            auto *action = instance.userInterfaceManager().lookupAction(id);
            require(action && dynamic_cast<SimpleAction *>(action) &&
                        !action->isCheckable() && action->hotkey().empty(),
                    "history menu action is missing or changes toggle state");
            if (ic) {
                require(ic->statusArea().isChild(action) ==
                            ic->statusArea().isChild(actions.front()),
                        "history menu visibility leaked into official input "
                        "method or vanished from LLM input method");
            }
        }
        auto *historyAction = instance.userInterfaceManager().lookupAction(
            "chinese-llm-history");
        const auto historyCount =
            std::to_string(llmAddon->requestHistory().entries().size());
        const auto historyLabel = historyAction->shortText(ic);
        require(historyLabel == "LLM request history " + historyCount ||
                    historyLabel == "LLM 请求历史 " + historyCount,
                "history menu count is not synchronized with retained rows");
    };
    auto clickStatistics = [&]() {
        const auto before = llmAddon->requestStatistics();
        const auto config = configSnapshot(llm->getConfig());
        const auto nativeConfig = configSnapshot(llm->getSubConfig("pinyin"));
        const auto *composition = ic->propertyAs<LlmState>("chinese-llm");
        const auto generationBefore = composition->generation;
        const auto seqBefore = composition->requestSeq;
        const auto pinyinBefore = composition->pinyin;
        const auto candidatesBefore = composition->candidates;
        const auto *debounceBefore = composition->debounce.get();
        const auto listBefore = ic->inputPanel().candidateList();
        const auto preeditBefore = preeditText(ic);
        const auto updatesBefore = statisticsStatusUpdates;
        for (auto *action : statisticsActions()) {
            action->activate(ic);
            action->activate(nullptr);
        }
        checkStatistics(before.sent, before.compared, before.overlapping);
        require(
            configSnapshot(llm->getConfig()) == config &&
                configSnapshot(llm->getSubConfig("pinyin")) == nativeConfig &&
                composition->generation == generationBefore &&
                composition->requestSeq == seqBefore &&
                composition->pinyin == pinyinBefore &&
                composition->candidates == candidatesBefore &&
                composition->debounce.get() == debounceBefore &&
                ic->inputPanel().candidateList() == listBefore &&
                preeditText(ic) == preeditBefore &&
                statisticsStatusUpdates == updatesBefore,
            "read-only statistics click changed configuration/composition/UI");
    };
    auto finish = [&](bool success, const std::string &message) {
        if (contiguousPayload && ic && llmAddon) {
            dumpComposition(ic, success ? "scenario-success" : "scenario-failure",
                            payloadScenario->second.first, true);
            for (const auto &entry : llmAddon->requestHistory().entries()) {
                std::cout << "request-diagnostic: "
                          << nlohmann::json{{"id", entry.id},
                                            {"seq", entry.seq},
                                            {"pinyin", entry.pinyin},
                                            {"status", fcitx::chinesellm::statusName(entry.status)},
                                            {"http_status", entry.httpStatus},
                                            {"elapsed_ms", entry.elapsedMs},
                                            {"content", entry.content},
                                            {"candidates", entry.candidates},
                                            {"error", entry.error}}
                                 .dump()
                          << std::endl;
            }
        }
        if (success) {
            struct ExpectedStatistics {
                uint64_t sent;
                uint64_t compared;
                uint64_t overlapping;
            };
            const std::map<std::string, ExpectedStatistics> expected{
                {"live", {1, 1, 1}},          {"mixed", {1, 1, 0}},
                {"mixed-context-changed", {1, 1, 0}},
                {"cache", {2, 2, 0}},         {"sensitive", {0, 0, 0}},
                {"dead", {1, 0, 0}},          {"toggle", {0, 0, 0}},
                {"history-menu", {1, 1, 1}},  {"stats-overlap", {1, 1, 1}},
                {"llm-alone", {1, 1, 1}}};
            const auto &counts = expected.at(scenario);
            checkStatistics(counts.sent, counts.compared, counts.overlapping);
            const auto &entries = llmAddon->requestHistory().entries();
            require(entries.size() <= 100 &&
                        llmAddon->historySnapshotPath().empty(),
                    "history exceeded its bound or implicitly wrote a "
                    "snapshot outside an explicit view");
            uint64_t previousId = 0;
            for (const auto &entry : entries) {
                require(entry.id > previousId && entry.timestampMs > 0 &&
                            !entry.pinyin.empty() && !entry.model.empty(),
                        "history rows lost identity, timestamp, keys or model");
                previousId = entry.id;
                require(entry.content.find("private-context-never-record") ==
                                std::string::npos &&
                            entry.error.find("history-private-key-sentinel") ==
                                std::string::npos,
                        "history retained request context or credentials");
            }
            auto entryWithStatus = [&](const std::string &status)
                -> const fcitx::chinesellm::RequestHistoryEntry & {
                for (const auto &entry : entries) {
                    if (fcitx::chinesellm::statusName(entry.status) == status) {
                        return entry;
                    }
                }
                throw std::runtime_error("missing history status: " + status);
            };
            if (payload || scenario == "live" || scenario == "llm-alone") {
                const auto &entry = entryWithStatus("usable");
                require(entries.size() == 1 && entry.started &&
                            entry.httpStatus == 200 && entry.seq != 0 &&
                            entry.elapsedMs >= 100 && !entry.content.empty() &&
                            !entry.candidates.empty() &&
                            entry.overlap == (counts.overlapping != 0) &&
                            entry.adoption == "llm" &&
                            entry.adoptedText ==
                                (payload ? payloadScenario->second.second
                                         : kModelWord),
                        "usable history lost timing/http/raw/overlap/LLM "
                        "adoption data");
            } else if (scenario == "stats-overlap") {
                const auto &entry = entryWithStatus("usable");
                require(entry.overlap == true && entry.adoption == "native" &&
                            entry.adoptedText == "你好",
                        "native equivalent candidate adoption was not recorded "
                        "separately from LLM selection");
            } else if (scenario == "dead") {
                const auto &entry = entryWithStatus("error");
                require(entry.started && !entry.error.empty() &&
                            entry.candidates.empty() &&
                            !entry.overlap.has_value() && entry.httpStatus == 0,
                        "error history lost transport/http/error information");
            } else if (scenario == "cache") {
                const auto &hit = entryWithStatus("cache_hit");
                require(entries.size() == 3 && &hit == &entries.back() &&
                            !hit.started && hit.httpStatus == 0 &&
                            hit.candidates == entries.front().candidates &&
                            counts.sent == 2 && counts.compared == 2,
                        "cache history was omitted or counted as a transfer");
            }
            if (scenario == "sensitive" || scenario == "history-menu" ||
                counts.sent == 0) {
                require(entries.empty(),
                        "disabled/cleared/sensitive path retained request "
                        "history");
            }
        }
        if (success && officialEngine && officialEntry && shuangpinEntry) {
            require(
                configSnapshot(officialEngine->getConfigForInputMethod(
                    *officialEntry)) == officialConfigBefore &&
                    configSnapshot(officialEngine->getConfigForInputMethod(
                        *shuangpinEntry)) == shuangpinConfigBefore,
                "LLM entry/runtime changed official pinyin/shuangpin settings");
        }
        if (success && alone) {
            for (const auto &forbidden :
                 {"pinyin", "punctuation", "fullwidth", "chttrans",
                  "pinyinhelper", "cloudpinyin", "quickphrase", "spell",
                  "imeapi", "clipboard"}) {
                require(!instance.addonManager().addon(forbidden),
                        std::string("private-only runtime lazily loaded "
                                    "official/shared addon: ") +
                            forbidden);
            }
        }
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
        std::string prefix;
        for (char c : keys) {
            const bool accepted = frontend->call<ITestFrontend::sendKeyEvent>(
                uuid, Key(std::string(1, c)), false);
            prefix.push_back(c);
            if (contiguousPayload) {
                dumpComposition(ic, keys == payloadScenario->second.first
                                        ? "initial-key"
                                        : "post-commit-key",
                                prefix, accepted);
                require(accepted, "contiguous mixed key was not accepted: " +
                                      prefix);
            }
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
            pinyin =
                alone ? nullptr : instance.addonManager().addon("pinyin", true);
            llm = instance.addonManager().addon("chinese-llm", true);
            require(frontend && llm && (alone || pinyin),
                    "required testfrontend/LLM and optional official pinyin "
                    "must load");
            require(
                instance.addonManager().addon("chinese-llm-punctuation") &&
                    instance.addonManager().addon("chinese-llm-fullwidth") &&
                    instance.addonManager().addon("chinese-llm-chttrans") &&
                    instance.addonManager().addon("chinese-llm-pinyinhelper"),
                "private helper dependencies did not load");
            if (alone) {
                require(!instance.addonManager().addon("cloudpinyin") &&
                            !instance.addonManager().addon("quickphrase") &&
                            !instance.addonManager().addon("spell") &&
                            !instance.addonManager().addon("imeapi") &&
                            !instance.addonManager().addon("clipboard"),
                        "LLM-alone loaded shared mutable optional modules");
                require(!instance.addonManager().addon("pinyin") &&
                            !instance.addonManager().addon("punctuation") &&
                            !instance.addonManager().addon("fullwidth") &&
                            !instance.addonManager().addon("chttrans") &&
                            !instance.addonManager().addon("pinyinhelper"),
                        "LLM-alone loaded an official mutable engine/helper");
            }
            officialEngine = dynamic_cast<InputMethodEngine *>(pinyin);
            llmEngine = dynamic_cast<InputMethodEngine *>(llm);
            // The runtime module is not linked into this executable; avoid an
            // out-of-line RTTI reference for its concrete addon type.
            llmAddon = static_cast<fcitx::chinesellm::ChineseLlm *>(llm);
            require((alone || officialEngine) && llmEngine && llmAddon,
                    "official pinyin and LLM addon must both be input method "
                    "engines");
            const auto *entry =
                instance.inputMethodManager().entry(kLlmInputMethod);
            officialEntry = instance.inputMethodManager().entry("pinyin");
            shuangpinEntry = instance.inputMethodManager().entry("shuangpin");
            require(
                entry && entry->uniqueName() == kLlmInputMethod &&
                    entry->addon() == "chinese-llm" &&
                    (entry->name() == "LLM拼音" ||
                     entry->nativeName() == "LLM拼音") &&
                    entry->isConfigurable(),
                "independent configurable LLM拼音 metadata was not discovered");
            require(alone ||
                        (officialEntry && officialEntry->addon() == "pinyin" &&
                         shuangpinEntry && shuangpinEntry->addon() == "pinyin"),
                    "independent entry replaced official pinyin/shuangpin "
                    "metadata");
            int llmEntries = 0;
            instance.inputMethodManager().foreachEntries(
                [&](const InputMethodEntry &candidate) {
                    if (candidate.uniqueName() == kLlmInputMethod) {
                        ++llmEntries;
                    }
                    return true;
                });
            require(llmEntries == 1,
                    "LLM拼音 must have exactly one discoverable entry");
            if (!alone) {
                officialConfigBefore = configSnapshot(
                    officialEngine->getConfigForInputMethod(*officialEntry));
                shuangpinConfigBefore = configSnapshot(
                    officialEngine->getConfigForInputMethod(*shuangpinEntry));
            }
            // Fcitx normally saves all loaded addons on exit. The external
            // smoke runner compares the persisted official file to this full
            // baseline, rather than incorrectly treating file creation as an
            // LLM configuration side effect.
            if (!alone) {
                std::cout << "official-config-baseline: "
                          << nlohmann::json(shuangpinConfigBefore).dump()
                          << '\n';
            }
            auto group = instance.inputMethodManager().currentGroup();
            group.inputMethodList().clear();
            // The installed keyboard engine may be statically linked into the
            // fcitx executable rather than available to an external test
            // binary.
            group.inputMethodList().emplace_back(kLlmInputMethod);
            if (!alone) {
                group.inputMethodList().emplace_back("pinyin");
                group.inputMethodList().emplace_back("shuangpin");
            }
            group.setDefaultInputMethod(initialInputMethod);
            instance.inputMethodManager().setGroup(std::move(group));

            RawConfig config;
            config.setValueByPath("Endpoint", endpoint);
            config.setValueByPath("Enabled", "True");
            config.setValueByPath("ShowBadge", "True");
            config.setValueByPath("CandidateIndex", "2");
            config.setValueByPath("Model", "mock");
            config.setValueByPath("ApiKey", "");
            config.setValueByPath("RecordHistory", "True");
            config.setValueByPath("DebounceMs", "250");
            config.setValueByPath("TimeoutSeconds", "1");
            config.setValueByPath("PromptTemplate", "{context}\n{pinyin}");
            config.setValueByPath(
                "ContextLength",
                scenario == "mixed-context-changed" ? "64" : "2");
            config.setValueByPath(
                "CacheSize",
                scenario == "cache" || scenario == "history-menu" ? "256"
                                                                  : "0");
            config.setValueByPath("MaxConsecutiveErrors", "2");
            llm->setConfig(config);
            checkStatistics(0, 0, 0);
            require(
                alone ||
                    (configSnapshot(officialEngine->getConfigForInputMethod(
                         *officialEntry)) == officialConfigBefore &&
                     configSnapshot(officialEngine->getConfigForInputMethod(
                         *shuangpinEntry)) == shuangpinConfigBefore),
                "LLM setup changed official pinyin/shuangpin configuration");
            uuid = frontend->call<ITestFrontend::createInputContext>(
                "testllmaddon");
            ic = instance.inputContextManager().findByUUID(uuid);
            require(ic, "failed to create input context");
            require(
                ic->property("chinese-llm-pinyinState") &&
                    (alone || (ic->property("pinyinState") &&
                               ic->property("pinyinState") !=
                                   ic->property("chinese-llm-pinyinState"))),
                "private and official pinyin input-context state are "
                "missing/shared");
            ic->setCapabilityFlags(CapabilityFlags{});
            if (scenario == "sensitive") {
                ic->setCapabilityFlags(CapabilityFlag::PasswordOrSensitive);
            }
            ic->focusIn();
            instance.setCurrentInputMethod(ic, initialInputMethod, true);
            require(scenario == "sensitive" ||
                        instance.inputMethod(ic) == initialInputMethod,
                    "requested input method is not active: " +
                        initialInputMethod);
            statisticsWatcher = instance.watchEvent(
                EventType::InputContextUpdateUI, EventWatcherPhase::Default,
                [&](Event &event) {
                    auto &update =
                        static_cast<InputContextUpdateUIEvent &>(event);
                    if (update.inputContext() == ic &&
                        update.component() ==
                            UserInterfaceComponent::StatusArea) {
                        ++statisticsStatusUpdates;
                    }
                });
            checkStatistics(0, 0, 0);
            clickStatistics();
            require(llmAddon->requestHistory().entries().empty() &&
                        llmAddon->historySnapshotPath().empty(),
                    "history must start empty and must not create a snapshot "
                    "before the user views it");
            if (scenario == "mixed-context-changed") {
                const std::string context = kContextBefore;
                ic->surroundingText().setText(context, context.size(),
                                              context.size());
                ic->updateSurroundingText();
            }
            if (scenario == "cache") {
                ic->surroundingText().setText("甲乙丙丁戊", 3, 3);
                ic->updateSurroundingText();
            }
            if (scenario == "llm-alone") {
                type("zzzz");
                require(
                    !hasCandidate(ic, "原生短语"),
                    "private-only engine read official customphrase sentinel");
                ic->reset();
            }
            start = stageStart = Clock::now();
            if (payload) {
                type(payloadScenario->second.first);
                if (contiguousPayload) {
                    dumpComposition(ic, "complete-input",
                                    payloadScenario->second.first, true);
                    require(
                        !preeditText(ic).empty() &&
                            normalizedTypedKeys(preeditText(ic)) ==
                                payloadScenario->second.first &&
                            !state()->pinyin.empty() &&
                            normalizedTypedKeys(state()->pinyin) ==
                                payloadScenario->second.first,
                        "complete contiguous input lost native preedit or "
                        "LLM state before final request: preedit=" +
                            preeditText(ic) + " state=" + state()->pinyin);
                }
                if (scenario == "mixed-context-changed") {
                    require(state()->context == kContextBefore &&
                                state()->debounce &&
                                state()->debounce->isEnabled() &&
                                state()->requestSeq == 0,
                            "changed-context probe must start before dispatch "
                            "with the old effective context");
                    contextChangedGeneration = state()->generation;
                    const std::string context = kContextAfter;
                    ic->surroundingText().setText(context, context.size(),
                                                  context.size());
                    // No explicit updateUserInterface here: a true context
                    // change must automatically retry the same complete keys.
                    ic->updateSurroundingText();
                    dumpComposition(ic, "effective-context-changed",
                                    payloadScenario->second.first, true);
                }
                require(!preeditText(ic).empty() &&
                            !hasCandidate(ic, payloadScenario->second.second),
                        "payload scenario must start with native composition "
                        "and no model candidate");
            } else {
                type(scenario == "stats-overlap" ? "nihao" : kKeys);
            }
            generation = state()->generation;
            if (scenario == "dead") {
                require(
                    !preeditText(ic).empty() &&
                        hasCandidate(ic, "他们正在开会"),
                    "disabled/unavailable LLM lost native dictionary fallback");
            }
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
                        if (scenario == "history-menu") {
                            auto *view =
                                instance.userInterfaceManager().lookupAction(
                                    "chinese-llm-history");
                            auto *clear =
                                instance.userInterfaceManager().lookupAction(
                                    "chinese-llm-history-clear");
                            if (stage == 0 && appeared) {
                                require(
                                    llmAddon->historySnapshotPath().empty() &&
                                        !std::filesystem::exists(
                                            openerLogPath()),
                                    "history wrote/launched before explicit "
                                    "menu activation");
                                const auto list =
                                    ic->inputPanel().candidateList();
                                const auto preedit = preeditText(ic);
                                const auto seq = state()->requestSeq;
                                const auto version = state()->generation;
                                view->activate(nullptr);
                                require(llmAddon->historySnapshotPath().empty(),
                                        "null history menu click wrote a "
                                        "snapshot");
                                view->activate(ic);
                                viewedSnapshot =
                                    llmAddon->historySnapshotPath();
                                checkPrivateSnapshot(viewedSnapshot);
                                const auto html = readTestData(viewedSnapshot);
                                require(
                                    html.find("history-html-marker") !=
                                            std::string::npos &&
                                        html.find("&lt;script&gt;") !=
                                            std::string::npos &&
                                        html.find("<script>") ==
                                            std::string::npos &&
                                        html.find("invalid_text") !=
                                            std::string::npos &&
                                        html.find("key_echo") !=
                                            std::string::npos &&
                                        html.find(kModelWord) !=
                                            std::string::npos,
                                    "HTML snapshot lost raw/reasons/candidates "
                                    "or executed model HTML");
                                require(
                                    ic->inputPanel().candidateList() == list &&
                                        preeditText(ic) == preedit &&
                                        state()->requestSeq == seq &&
                                        state()->generation == version,
                                    "viewing history changed composition");
                                checkStatistics(1, 1, 1);
                                stage = 1;
                            } else if (stage == 1 && openerRecorded()) {
                                const auto opener = nlohmann::json::parse(
                                    readTestData(openerLogPath()));
                                require(opener.at("args").size() == 1 &&
                                            opener.at("path").get<std::string>() ==
                                                viewedSnapshot &&
                                            opener.at("file_mode") == 0600 &&
                                            opener.at("directory_mode") == 0700,
                                        "menu did not invoke isolated opener "
                                        "with private snapshot path");
                                const auto beforeAdoption =
                                    readTestData(viewedSnapshot);
                                frontend->call<
                                    ITestFrontend::pushCommitExpectation>(
                                    kModelWord);
                                selectCandidate(ic, kModelWord);
                                require(llmAddon->requestHistory()
                                                .entries()
                                                .back()
                                                .adoption == "llm",
                                        "adoption was not recorded");
                                // Snapshot rewrites are coalesced on a
                                // timer; an opened page catches up shortly.
                                snapshotBeforeAdoption = beforeAdoption;
                                stage = 2;
                                stageStart = Clock::now();
                            } else if (stage == 2 &&
                                       readTestData(viewedSnapshot) !=
                                           snapshotBeforeAdoption) {
                                require(stageElapsed <
                                            std::chrono::seconds(2),
                                        "adoption refreshed the viewed snapshot "
                                        "too late");
                                type(kKeys);
                                const auto &entries =
                                    llmAddon->requestHistory().entries();
                                require(hasCandidate(ic, kModelWord) &&
                                            entries.size() == 2 &&
                                            entries.back().status ==
                                                fcitx::chinesellm::RequestStatus::CacheHit &&
                                            !entries.back().started,
                                        "viewed history missed cache-hit row "
                                        "or cache failed after commit");
                                const auto list =
                                    ic->inputPanel().candidateList();
                                const auto preedit = preeditText(ic);
                                const auto pinyin = state()->pinyin;
                                const auto candidates = state()->candidates;
                                const auto seq = state()->requestSeq;
                                const auto version = state()->generation;
                                auto requireCompositionUnchanged = [&]() {
                                    require(
                                        ic->inputPanel().candidateList() ==
                                                list &&
                                            preeditText(ic) == preedit &&
                                            state()->pinyin == pinyin &&
                                            state()->candidates == candidates &&
                                            state()->requestSeq == seq &&
                                            state()->generation == version,
                                        "history clear/record-only toggle "
                                        "destroyed current composition");
                                    checkStatistics(1, 1, 1);
                                };
                                clear->activate(nullptr);
                                require(entries.size() == 2,
                                        "null clear-history menu click cleared "
                                        "active history");
                                clear->activate(ic);
                                require(
                                    entries.empty() &&
                                        llmAddon->historySnapshotPath().empty() &&
                                        !std::filesystem::exists(
                                            viewedSnapshot) &&
                                        !std::filesystem::exists(
                                            std::filesystem::path(
                                                viewedSnapshot)
                                                .parent_path()),
                                    "clear history retained rows/snapshot/private "
                                    "directory");
                                requireCompositionUnchanged();
                                for (const auto *enabled : {"False", "True"}) {
                                    RawConfig recording;
                                    recording.setValueByPath("RecordHistory",
                                                             enabled);
                                    llm->setConfig(recording);
                                    require(entries.empty() &&
                                                llmAddon->historySnapshotPath()
                                                    .empty(),
                                            "record toggle resurrected old "
                                            "rows or snapshot");
                                    requireCompositionUnchanged();
                                }
                                checkCommit(kModelWord);
                                finish(true, "");
                            }
                        } else if (payload) {
                            const auto &expectedWord =
                                payloadScenario->second.second;
                            if (hasCandidate(ic, expectedWord)) {
                                if (scenario == "mixed-context-changed") {
                                    const auto &entries =
                                        llmAddon->requestHistory().entries();
                                    require(
                                        state()->context == kContextAfter &&
                                            state()->generation >
                                                contextChangedGeneration &&
                                            entries.size() == 1 &&
                                            state()->requestSeq == 0 &&
                                            entries.back().seq != 0 &&
                                            entries.back().content.find(
                                                kContextBefore) ==
                                                std::string::npos &&
                                            entries.back().content.find(
                                                kContextAfter) ==
                                                std::string::npos &&
                                            entries.back().error.find(
                                                kContextAfter) ==
                                                std::string::npos,
                                        "context change did not retry current "
                                        "keys automatically or retained old "
                                        "request/context history");
                                }
                                if (contiguousPayload) {
                                    const auto &entries =
                                        llmAddon->requestHistory().entries();
                                    require(
                                        !entries.empty() &&
                                            normalizedTypedKeys(
                                                entries.back().pinyin) ==
                                                payloadScenario->second.first &&
                                            entries.back().status == fcitx::chinesellm::RequestStatus::Usable &&
                                            entries.back().started &&
                                            entries.back().httpStatus == 200 &&
                                            entries.back().content ==
                                                expectedWord &&
                                            normalizedTypedKeys(state()->pinyin) ==
                                                payloadScenario->second.first,
                                        "visible mixed candidate lacks a "
                                        "complete final-input HTTP/history row");
                                    dumpComposition(
                                        ic, "final-candidate-visible",
                                        payloadScenario->second.first, true);
                                }
                                checkStatistics(1, 1, 0);
                                require(
                                    state()->candidates ==
                                        std::vector<std::string>{expectedWord},
                                    "worker/addon changed semantic candidate "
                                    "bytes or imposed pure-Han length limit");
                                const bool badge =
                                    scenario != "mixed-badge-off";
                                auto checkPayloadBadge = [&]() {
                                    auto list =
                                        ic->inputPanel().candidateList();
                                    auto *bulk = list ? list->toBulk() : nullptr;
                                    require(bulk,
                                            "payload has no bulk candidate "
                                            "list");
                                    int matches = 0;
                                    for (int i = 0; i < bulk->totalSize(); ++i) {
                                        const auto &word =
                                            bulk->candidateFromAll(i);
                                        if (word.text().toString() !=
                                            expectedWord) {
                                            require(
                                                word.comment().toString().find(
                                                    "\u2601") ==
                                                    std::string::npos,
                                                "payload cloud badge leaked "
                                                "into native candidates");
                                            continue;
                                        }
                                        ++matches;
                                        require(
                                            word.comment().toString() ==
                                                (badge ? "\u2601\uFE0E" : ""),
                                            "payload badge was lost or "
                                            "included in semantic text");
                                        const auto displayed =
                                            instance
                                                .outputFilter(
                                                    ic,
                                                    word.textWithComment(
                                                        word.spaceBetweenComment()
                                                            ? " "
                                                            : ""))
                                                .toString();
                                        require(
                                            displayed ==
                                                expectedWord +
                                                    (badge
                                                         ? " \u2601\uFE0E"
                                                         : ""),
                                            "display filtering changed "
                                            "English/emoji/punctuation bytes");
                                    }
                                    require(matches == 1,
                                            "payload candidate duplicated "
                                            "during UI refresh");
                                };
                                checkPayloadBadge();
                                ic->updateUserInterface(
                                    UserInterfaceComponent::InputPanel);
                                checkPayloadBadge();
                                clickStatistics();
                                const auto oldGeneration = state()->generation;
                                // The real frontend expectation compares the
                                // entire commit string byte-for-byte, including
                                // spaces, VS, ZWJ, modifiers and tag characters.
                                frontend->call<
                                    ITestFrontend::pushCommitExpectation>(
                                    expectedWord);
                                selectCandidate(ic, expectedWord);
                                require(
                                    ic->inputPanel().clientPreedit().empty() &&
                                        ic->inputPanel().preedit().empty() &&
                                        state()->pinyin.empty() &&
                                        state()->context.empty() &&
                                        state()->candidates.empty() &&
                                        state()->requestSeq == 0 &&
                                        !state()->debounce &&
                                        state()->generation > oldGeneration &&
                                        !hasCandidate(ic, expectedWord),
                                    "payload commit did not reset native/LLM "
                                    "composition or left stale candidate");
                                type("shui");
                                require(preeditText(ic) == "shui" &&
                                            !hasCandidate(ic, expectedWord),
                                        "payload commit retained old native "
                                        "composition/badge");
                                ic->reset();
                                finish(true, "");
                            }
                        } else if (scenario == "stats-overlap") {
                            const auto counts = llmAddon->requestStatistics();
                            if (counts.compared == 1) {
                                checkStatistics(1, 1, 1);
                                require(
                                    state()->candidates ==
                                            std::vector<std::string>{"你好"} &&
                                        hasCandidate(ic, "你好"),
                                    "all-overlap reply lost native fallback");
                                auto *bulk =
                                    ic->inputPanel().candidateList()->toBulk();
                                int matches = 0;
                                for (int i = 0; i < bulk->totalSize(); ++i) {
                                    const auto &word =
                                        bulk->candidateFromAll(i);
                                    if (word.text().toString() == "你好") {
                                        ++matches;
                                        require(word.comment().toString().find(
                                                    "\u2601") ==
                                                    std::string::npos,
                                                "all-overlap native word was "
                                                "marked as LLM");
                                    }
                                }
                                require(
                                    matches == 1,
                                    "all-overlap reply duplicated native word");
                                ic->updateUserInterface(
                                    UserInterfaceComponent::InputPanel);
                                clickStatistics();
                                checkStatistics(1, 1, 1);
                                checkCommit("你好");
                                finish(true, "");
                            }
                        } else if (scenario == "dead" ||
                                   scenario == "sensitive" ||
                                   scenario == "toggle") {
                            require(!appeared, "unexpected model candidate");
                            if (elapsed > std::chrono::milliseconds(1800)) {
                                require(!state()->debounce ||
                                            !state()->debounce->isEnabled(),
                                        "unexpected pending debounce");
                                if (scenario == "dead") {
                                    require(hasCandidate(ic, "他们正在开会"),
                                            "native fallback vanished while "
                                            "waiting for LLM");
                                    checkCommit("他们正在开会");
                                }
                                finish(true, "");
                            }
                        } else if (scenario == "cache") {
                            // Different surrounding text must not reuse a
                            // cached result; the same text must hit the cache.
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
                                ic->reset();
                                ic->surroundingText().setCursor(3, 3);
                                ic->updateSurroundingText();
                                type(kKeys);
                                require(hasCandidate(ic, kModelWord),
                                        "expected synchronous cache hit");
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
