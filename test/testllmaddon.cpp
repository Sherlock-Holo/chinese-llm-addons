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
#include <iostream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fcitx;
using fcitx::chinesellm::LlmState;

namespace {
constexpr char kLlmInputMethod[] = "llm-pinyin";
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
std::string configValue(const Configuration *config, const std::string &path) {
    require(config, "missing configuration for " + path);
    RawConfig saved;
    config->save(saved);
    const auto *value = saved.valueByPath(path);
    require(value, "missing configuration value " + path);
    return *value;
}

void checkNativeDescription(AddonInstance *llm) {
    const auto *native = llm->getSubConfig("pinyin");
    require(native && native != llm->getConfig(),
            "native settings must have an independent subconfig");
    RawConfig root;
    llm->getConfig()->dumpDescription(root);
    const auto rootPath =
        std::string(llm->getConfig()->typeName()) + "/NativePinyin/";
    require(
        root.valueByPath(rootPath + "Type") &&
            *root.valueByPath(rootPath + "Type") == "External" &&
            root.valueByPath(rootPath + "External") &&
            *root.valueByPath(rootPath + "External") ==
                "fcitx://config/addon/chinese-llm/pinyin" &&
            root.valueByPath(rootPath + "LaunchSubConfig") &&
            *root.valueByPath(rootPath + "LaunchSubConfig") == "True",
        "root GUI description does not link private native pinyin settings");
    RawConfig raw;
    native->dumpDescription(raw);
    const std::string type = native->typeName();
    const auto rawType = raw.get(type);
    require(rawType && rawType->subItemsSize() > 10,
            "native GUI description is empty");
    for (const auto &[field, expected] :
         std::vector<std::pair<std::string, std::string>>{
             {"PageSize", "Integer"},
             {"Prediction", "Boolean"},
             {"PreeditMode", "Enum"},
             {"SecondCandidate", "List|Key"}}) {
        const auto *value = raw.valueByPath(type + '/' + field + "/Type");
        require(value && *value == expected,
                "native GUI field/type missing: " + field);
    }
    const auto *fuzzyType = raw.valueByPath(type + "/Fuzzy/Type");
    require(fuzzyType && raw.valueByPath(*fuzzyType + "/NG_GN/Type"),
            "native fuzzy settings have no nested GUI description");
    raw.visitSubItems([&](const RawConfig &field, const std::string &path) {
        if (path.ends_with("/External")) {
            for (const auto &forbidden :
                 {"addon/pinyin", "addon/punctuation", "addon/fullwidth",
                  "addon/chttrans", "addon/cloudpinyin", "addon/quickphrase",
                  "addon/spell", "addon/pinyinhelper"}) {
                require(field.value().find(forbidden) == std::string::npos,
                        "native GUI exposes shared mutable addon URI: " +
                            field.value());
            }
        }
        return true;
    });
    const auto dbus = dumpDBusConfigDescription(*native);
    const auto entry = std::ranges::find_if(dbus, [&](const auto &section) {
        return std::get<0>(section) == type;
    });
    require(entry != dbus.end() &&
                std::get<1>(*entry).size() == rawType->subItemsSize(),
            "native GUI D-Bus description lost options");
    for (const auto &option : std::get<1>(*entry)) {
        const auto *rawFieldType =
            raw.valueByPath(type + '/' + std::get<0>(option) + "/Type");
        require(rawFieldType && *rawFieldType == std::get<1>(option) &&
                    !std::get<2>(option).empty(),
                "native GUI D-Bus option type/description does not match raw "
                "schema");
    }
}

std::filesystem::path testDataHome() {
    const char *home = std::getenv("FCITX_DATA_HOME");
    require(home && *home, "isolated FCITX_DATA_HOME must be set");
    return home;
}

void writeTestData(const std::filesystem::path &path,
                   const std::string &value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << value;
    require(static_cast<bool>(stream),
            "cannot write isolated test data " + path.string());
}

std::string readTestData(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream),
            "missing isolated data " + path.string());
    return std::string(std::istreambuf_iterator<char>(stream), {});
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
                                             "filtered",
                                             "metadata",
                                             "original-pinyin",
                                             "original-shuangpin",
                                             "switch-inflight",
                                             "switch-results",
                                             "own-config",
                                             "virtual-keyboard",
                                             "preedit-click",
                                             "modifier-selection",
                                             "modifier-selection-global",
                                             "llm-alone",
                                             "private-settings",
                                             "customphrase-isolation",
                                             "learning-isolation",
                                             "learning-reload",
                                             "private-actions",
                                             "native-description",
                                             "cloud-badge",
                                             "badge-off",
                                             "llm-status"};
    if (std::ranges::find(scenarios, scenario) == scenarios.end()) {
        std::cerr << "unknown scenario: " << scenario << '\n';
        return EXIT_FAILURE;
    }
    Log::setLogRule("default=3,chinese-llm=4");
    char arg0[] = "testllmaddon";
    char arg1[] = "--disable=all";
    const bool alone = scenario == "llm-alone" ||
                       scenario == "learning-isolation" ||
                       scenario == "learning-reload";
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
    AddonInstance *pinyin = nullptr;
    ICUUID uuid{};
    Clock::time_point start;
    Clock::time_point stageStart;
    std::unique_ptr<EventSourceTime> poll;
    std::shared_ptr<CandidateList> retainedList;
    int retainedModelIndex = -1;
    std::string officialConfigBefore;
    std::string shuangpinConfigBefore;
    const InputMethodEntry *officialEntry = nullptr;
    const InputMethodEntry *shuangpinEntry = nullptr;
    InputMethodEngine *officialEngine = nullptr;
    InputMethodEngine *llmEngine = nullptr;
    const bool originalPinyin = scenario == "original-pinyin";
    const bool originalShuangpin = scenario == "original-shuangpin";
    const bool original = originalPinyin || originalShuangpin;
    const bool modifierSelection = scenario == "modifier-selection" ||
                                   scenario == "modifier-selection-global";
    const std::string initialInputMethod = originalPinyin ? "pinyin"
                                           : originalShuangpin
                                               ? "shuangpin"
                                               : kLlmInputMethod;

    auto finish = [&](bool success, const std::string &message) {
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
            require((alone || officialEngine) && llmEngine,
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
            if (modifierSelection) {
                // Explicitly configure the native engine inside this scenario's
                // isolated home; the LLM engine must delegate this
                // modifier-only selection even though native PreInputMethod
                // watches pinyin only.
                RawConfig nativeSelection;
                nativeSelection.setValueByPath("SecondCandidate/0", "Shift_L");
                llm->setSubConfig("pinyin", nativeSelection);
            }
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
            if (scenario == "modifier-selection-global") {
                // Force a genuinely applicable global shortcut to conflict with
                // native second-candidate selection. Without a wrapper
                // PreInputMethod handler, enumeration consumes Shift before the
                // delegated engine.
                RawConfig global;
                global.setValueByPath("Hotkey/EnumerateForwardKeys/0",
                                      "Shift_L");
                global.setValueByPath("Hotkey/EnumerateSkipFirst", "False");
                instance.globalConfig().load(global, true);
                const auto &keys =
                    instance.globalConfig().enumerateForwardKeys();
                require(
                    keys.size() == 1 && keys.front() == Key("Shift_L") &&
                        !instance.globalConfig().enumerateSkipFirst(),
                    "global enumeration conflict shortcut was not configured");
            }

            RawConfig config;
            config.setValueByPath("Endpoint", endpoint);
            config.setValueByPath(
                "Enabled", scenario == "disabled" ||
                                   scenario == "private-settings" ||
                                   scenario == "customphrase-isolation" ||
                                   scenario == "learning-isolation" ||
                                   scenario == "learning-reload" ||
                                   scenario == "private-actions" ||
                                   scenario == "native-description"
                               ? "False"
                               : "True");
            config.setValueByPath("ShowBadge",
                                  scenario == "badge-off" ? "False" : "True");
            config.setValueByPath("CandidateIndex",
                                  scenario == "cloud-badge" ? "1" : "2");
            config.setValueByPath("Model", "mock");
            config.setValueByPath("ApiKey", "");
            config.setValueByPath("DebounceMs", "250");
            config.setValueByPath("TimeoutSeconds", "1");
            config.setValueByPath("PromptTemplate", "{context}\n{pinyin}");
            config.setValueByPath("ContextLength", "2");
            config.setValueByPath("CacheSize",
                                  scenario == "cache" || scenario == "config" ||
                                          scenario == "switch-results"
                                      ? "256"
                                      : "0");
            config.setValueByPath("MaxConsecutiveErrors", "2");
            llm->setConfig(config);
            require(
                alone ||
                    (configSnapshot(officialEngine->getConfigForInputMethod(
                         *officialEntry)) == officialConfigBefore &&
                     configSnapshot(officialEngine->getConfigForInputMethod(
                         *shuangpinEntry)) == shuangpinConfigBefore),
                "LLM setup changed official pinyin/shuangpin configuration");
            if (scenario == "metadata" || scenario == "own-config") {
                require(llmEngine->getConfigForInputMethod(*entry) ==
                            llm->getConfig(),
                        "LLM input-method gear must expose only its own "
                        "configuration");
                if (scenario == "own-config") {
                    RawConfig ownChange;
                    ownChange.setValueByPath("Model", "own-gear-model");
                    ownChange.setValueByPath("Enabled", "False");
                    llmEngine->setConfigForInputMethod(*entry, ownChange);
                    RawConfig saved;
                    llmEngine->getConfigForInputMethod(*entry)->save(saved);
                    const auto model = saved.get("Model");
                    const auto enabled = saved.get("Enabled");
                    require(
                        model && model->value() == "own-gear-model" &&
                            enabled && enabled->value() == "False",
                        "LLM input-method gear failed to update own settings");
                    llm->reloadConfig();
                    require(configSnapshot(
                                llmEngine->getConfigForInputMethod(*entry))
                                    .find("Model=own-gear-model") !=
                                std::string::npos,
                            "LLM input-method gear settings did not persist");
                }
                require(
                    configSnapshot(officialEngine->getConfigForInputMethod(
                        *officialEntry)) == officialConfigBefore &&
                        configSnapshot(officialEngine->getConfigForInputMethod(
                            *shuangpinEntry)) == shuangpinConfigBefore,
                    "LLM gear configuration changed official pinyin/shuangpin "
                    "settings");
                finish(true, "");
                return;
            }
            if (scenario == "preview" || scenario == "preedit-click") {
                RawConfig preview;
                preview.setValueByPath("PreeditMode", scenario == "preview"
                                                          ? "Commit preview"
                                                          : "Composing pinyin");
                llm->setSubConfig("pinyin", preview);
            }
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
            ic->setCapabilityFlags(
                scenario == "preview" || scenario == "preedit-click"
                    ? CapabilityFlags(CapabilityFlag::Preedit)
                    : CapabilityFlags{});
            if (scenario == "sensitive") {
                ic->setCapabilityFlags(CapabilityFlag::PasswordOrSensitive);
            }
            ic->focusIn();
            instance.setCurrentInputMethod(ic, initialInputMethod, true);
            require(scenario == "sensitive" ||
                        instance.inputMethod(ic) == initialInputMethod,
                    "requested input method is not active: " +
                        initialInputMethod);
            if (scenario == "llm-status") {
                auto *action = instance.userInterfaceManager().lookupAction(
                    "chinese-llm-toggle");
                require(action && action->isCheckable() &&
                            ic->statusArea().isChild(action),
                        "LLM toggle action is not visible in its status menu");
                int statusUpdates = 0;
                auto statusWatcher = instance.watchEvent(
                    EventType::InputContextUpdateUI, EventWatcherPhase::Default,
                    [&](Event &event) {
                        auto &update =
                            static_cast<InputContextUpdateUIEvent &>(event);
                        if (update.inputContext() == ic &&
                            update.component() ==
                                UserInterfaceComponent::StatusArea) {
                            ++statusUpdates;
                        }
                    });
                auto checkStatus = [&](bool enabled) {
                    const auto label = action->shortText(ic);
                    require(configValue(llm->getConfig(), "Enabled") ==
                                    (enabled ? "True" : "False") &&
                                action->isChecked(ic) == enabled &&
                                (enabled
                                     ? (label == "LLM candidates enabled" ||
                                        label == "LLM 候选词已启用")
                                     : (label == "LLM candidates disabled" ||
                                        label == "LLM 候选词已停用")) &&
                                llmEngine->subModeLabel(*entry, *ic) ==
                                    (enabled ? "AI" : "PY"),
                            "menu, checkbox, top-bar label and configuration "
                            "disagree");
                    require(label.find(':') == std::string::npos &&
                                action->longText(ic).find(':') ==
                                    std::string::npos,
                            "LLM status fields contain Kimpanel protocol "
                            "delimiter");
                };
                checkStatus(true);
                action->activate(ic);
                checkStatus(false);
                require(statusUpdates > 0,
                        "menu toggle did not request StatusArea refresh");
                llm->reloadConfig();
                checkStatus(false);
                const int beforeKey = statusUpdates;
                require(frontend->call<ITestFrontend::sendKeyEvent>(
                            uuid, Key("Control+Alt+Shift+L"), false),
                        "LLM toggle key was not accepted");
                checkStatus(true);
                require(statusUpdates > beforeKey,
                        "keyboard toggle did not refresh Kimpanel state");
                RawConfig change;
                change.setValueByPath("Enabled", "False");
                change.setValueByPath("ToggleKey/0", "Control+Alt+Shift+K");
                const int beforeConfig = statusUpdates;
                llm->setConfig(change);
                checkStatus(false);
                require(statusUpdates > beforeConfig &&
                            Key("Control+Alt+Shift+K")
                                .checkKeyList(action->hotkey()),
                        "GUI config did not update status/hotkey metadata");
                frontend->call<ITestFrontend::sendKeyEvent>(
                    uuid, Key("Control+Alt+Shift+L"), false);
                checkStatus(false);
                frontend->call<ITestFrontend::sendKeyEvent>(
                    uuid, Key("Control+Alt+Shift+K"), false);
                checkStatus(true);
                ic->focusOut();
                // GNOME shell menus can transiently take keyboard focus. The
                // selected IM's action must still work on the most recent IC.
                require(ic->statusArea().isChild(action),
                        "LLM menu action vanished during transient focus loss");
                action->activate(ic);
                checkStatus(false);
                action->activate(ic);
                checkStatus(true);
                const auto beforeStaleClick = configSnapshot(llm->getConfig());
                action->activate(nullptr);
                require(configSnapshot(llm->getConfig()) == beforeStaleClick,
                        "null menu click toggled LLM");
                ic->focusIn();
                for (const auto *nativeEntry :
                     {officialEntry, shuangpinEntry}) {
                    instance.setCurrentInputMethod(
                        ic, nativeEntry->uniqueName(), true);
                    require(
                        !ic->statusArea().isChild(action),
                        "LLM toggle action leaked into official input method");
                    action->activate(ic);
                    require(
                        configSnapshot(llm->getConfig()) == beforeStaleClick,
                        "stale LLM action toggled from official input method");
                }
                instance.setCurrentInputMethod(ic, kLlmInputMethod, true);
                require(ic->statusArea().isChild(action),
                        "LLM action was not restored after switching back");
                checkStatus(true);
                finish(true, "");
                return;
            }
            if (scenario == "native-description") {
                checkNativeDescription(llm);
                require(ic->property("chinese-llm-pinyinState"),
                        "native engine did not register private input-context "
                        "state");
                finish(true, "");
                return;
            }
            if (scenario == "private-settings") {
                const auto ownRoot = configSnapshot(llm->getConfig());
                const auto *native = llm->getSubConfig("pinyin");
                require(native, "private native settings are unavailable");
                const auto officialFuzzy = configValue(
                    officialEngine->getConfigForInputMethod(*shuangpinEntry),
                    "Fuzzy/NG_GN");
                const auto oppositeFuzzy =
                    officialFuzzy == "True" ? "False" : "True";
                const auto officialPage = configValue(
                    officialEngine->getConfigForInputMethod(*shuangpinEntry),
                    "PageSize");
                const auto privatePage = officialPage == "3" ? "5" : "3";
                RawConfig change;
                change.setValueByPath("PageSize", privatePage);
                change.setValueByPath("Fuzzy/NG_GN", oppositeFuzzy);
                change.setValueByPath("Prediction", "False");
                llm->setSubConfig("pinyin", change);
                require(configValue(native, "PageSize") == privatePage &&
                            configValue(native, "Fuzzy/NG_GN") == oppositeFuzzy,
                        "independent native config did not apply page/fuzzy "
                        "options");
                llm->reloadConfig();
                require(configValue(llm->getSubConfig("pinyin"), "PageSize") ==
                                privatePage &&
                            configValue(llm->getSubConfig("pinyin"),
                                        "Fuzzy/NG_GN") == oppositeFuzzy &&
                            configSnapshot(llm->getConfig()) == ownRoot,
                        "private native settings did not persist or overwrote "
                        "LLM root");
                type("nihao");
                auto *privatePageable = dynamic_cast<CommonCandidateList *>(
                    ic->inputPanel().candidateList().get());
                require(privatePageable && privatePageable->pageSize() ==
                                               std::stoi(privatePage),
                        "private PageSize was not used by native candidates");
                ic->reset();
                instance.setCurrentInputMethod(ic, "pinyin", true);
                type("nihao");
                auto *officialPageable = dynamic_cast<CommonCandidateList *>(
                    ic->inputPanel().candidateList().get());
                require(officialPageable && officialPageable->pageSize() ==
                                                std::stoi(officialPage),
                        "private PageSize leaked to ordinary pinyin");
                ic->reset();
                finish(true, "");
                return;
            }
            if (scenario == "customphrase-isolation") {
                const auto officialPhrase =
                    testDataHome() / "pinyin/customphrase";
                const auto privatePhrase =
                    testDataHome() / "chinese-llm/customphrase";
                const auto original = readTestData(officialPhrase);
                writeTestData(privatePhrase, "zzzz,1=私有短语\n");
                llm->setSubConfig("pinyin/customphrase", RawConfig{});
                type("zzzz");
                require(hasCandidate(ic, "私有短语") &&
                            !hasCandidate(ic, "原生短语"),
                        "private engine read the official custom phrase or "
                        "ignored its own");
                frontend->call<ITestFrontend::pushCommitExpectation>(
                    "私有短语");
                selectCandidate(ic, "私有短语");
                require(preeditText(ic).empty(),
                        "private customphrase commit did not reset");
                writeTestData(privatePhrase, "zzzz,1=私有更新\n");
                llm->setSubConfig("pinyin/customphrase", RawConfig{});
                type("zzzz");
                require(hasCandidate(ic, "私有更新") &&
                            !hasCandidate(ic, "私有短语"),
                        "private customphrase reload used shared/stale state");
                ic->reset();
                instance.setCurrentInputMethod(ic, "pinyin", true);
                type("zzzz");
                require(hasCandidate(ic, "原生短语") &&
                            !hasCandidate(ic, "私有更新"),
                        "private customphrase leaked into ordinary pinyin");
                require(readTestData(officialPhrase) == original,
                        "private phrase update rewrote official file");
                ic->reset();
                finish(true, "");
                return;
            }
            if (scenario == "learning-isolation") {
                require(!instance.addonManager().addon("pinyin"),
                        "learning test loaded official engine");
                llm->setSubConfig("pinyin/clearalldict", RawConfig{});
                llm->save();
                const auto dict = testDataHome() / "chinese-llm/user.dict";
                const auto history =
                    testDataHome() / "chinese-llm/user.history";
                const auto emptyDict = readTestData(dict);
                const auto emptyHistory = readTestData(history);
                type("nihao");
                require(hasCandidate(ic, "你好"),
                        "private dictionary fallback missing before learning");
                frontend->call<ITestFrontend::pushCommitExpectation>("你好");
                selectCandidate(ic, "你好");
                ic->reset();
                llm->save();
                require(std::filesystem::file_size(dict) > 0 &&
                            std::filesystem::file_size(history) > 0,
                        "private learned dict/history were not persisted");
                const auto learnedDict = readTestData(dict);
                const auto learnedHistory = readTestData(history);
                require(learnedDict != emptyDict ||
                            learnedHistory != emptyHistory,
                        "private native commit did not learn any "
                        "dictionary/history data");
                llm->reloadConfig();
                type("nihao");
                require(hasCandidate(ic, "你好"),
                        "private native engine lost learned word on reload");
                ic->reset();
                llm->save();
                require(readTestData(dict) == learnedDict &&
                            readTestData(history) == learnedHistory,
                        "private learning changed unexpectedly across "
                        "save/reload without new commit");
                finish(true, "");
                return;
            }
            if (scenario == "learning-reload") {
                // smoke.sh seeds these files from the preceding
                // private-learning process. This new engine construction must
                // read them, not just preserve in-memory state across
                // configuration reload.
                const auto dict = testDataHome() / "chinese-llm/user.dict";
                const auto history =
                    testDataHome() / "chinese-llm/user.history";
                const auto persistedDict = readTestData(dict);
                const auto persistedHistory = readTestData(history);
                type("nihao");
                require(hasCandidate(ic, "你好"),
                        "fresh private engine could not use learned data");
                ic->reset();
                llm->save();
                require(readTestData(dict) == persistedDict &&
                            readTestData(history) == persistedHistory,
                        "fresh private engine ignored/damaged persisted "
                        "learning data");
                finish(true, "");
                return;
            }
            if (scenario == "private-actions") {
                auto *privatePunctuation =
                    instance.addonManager().addon("chinese-llm-punctuation");
                auto *officialPunctuation =
                    instance.addonManager().addon("punctuation", true);
                require(privatePunctuation && officialPunctuation &&
                            privatePunctuation->getSubConfig(
                                "punctuationmap/zh_CN") &&
                            officialPunctuation->getSubConfig(
                                "punctuationmap/zh_CN"),
                        "private/official punctuation resource profiles were "
                        "not loaded");
                const auto officialMap = configSnapshot(
                    officialPunctuation->getSubConfig("punctuationmap/zh_CN"));
                frontend->call<ITestFrontend::pushCommitExpectation>("，");
                require(frontend->call<ITestFrontend::sendKeyEvent>(
                            uuid, Key(","), false),
                        "private punctuation did not convert default comma");
                RawConfig privateMap;
                privateMap.setValueByPath("Entries/0/Key", ",");
                privateMap.setValueByPath("Entries/0/Mapping", "、");
                privatePunctuation->setSubConfig("punctuationmap/zh_CN",
                                                 privateMap);
                privatePunctuation->reloadConfig();
                frontend->call<ITestFrontend::pushCommitExpectation>("、");
                require(frontend->call<ITestFrontend::sendKeyEvent>(
                            uuid, Key(","), false),
                        "private punctuation custom map/reload did not apply");
                require(
                    configSnapshot(officialPunctuation->getSubConfig(
                        "punctuationmap/zh_CN")) == officialMap,
                    "private punctuation map update changed official profile");
                const auto fullOfficial = configSnapshot(
                    officialEngine->getConfigForInputMethod(*shuangpinEntry));
                const auto nativeRoot = llm->getSubConfig("pinyin");
                const auto beforePrediction =
                    configValue(nativeRoot, "Prediction");
                auto *prediction = instance.userInterfaceManager().lookupAction(
                    "chinese-llm-pinyin-prediction");
                auto *officialPrediction =
                    instance.userInterfaceManager().lookupAction(
                        "pinyin-prediction");
                require(prediction && officialPrediction &&
                            prediction != officialPrediction &&
                            ic->statusArea().isChild(prediction),
                        "private prediction action is missing/shared/not in "
                        "active status area");
                prediction->activate(ic);
                require(
                    configValue(nativeRoot, "Prediction") != beforePrediction &&
                        configSnapshot(officialEngine->getConfigForInputMethod(
                            *shuangpinEntry)) == fullOfficial,
                    "private prediction action toggled official state");
                prediction->activate(ic);
                for (const auto &helper :
                     {"punctuation", "fullwidth", "chttrans"}) {
                    require(
                        instance.addonManager().addon(helper, true),
                        "official helper failed to load for isolation test");
                    auto *privateAction =
                        instance.userInterfaceManager().lookupAction(
                            std::string("chinese-llm-") + helper);
                    auto *officialAction =
                        instance.userInterfaceManager().lookupAction(helper);
                    require(
                        privateAction &&
                            ic->statusArea().isChild(privateAction) &&
                            officialAction && privateAction != officialAction,
                        std::string("private helper action missing/shared: ") +
                            helper);
                    const auto originalText =
                        officialAction ? officialAction->shortText(ic) : "";
                    privateAction->activate(ic);
                    require(
                        !officialAction ||
                            officialAction->shortText(ic) == originalText,
                        std::string(
                            "private helper action changed global helper: ") +
                            helper);
                    privateAction->activate(ic);
                }
                ic->reset();
                instance.setCurrentInputMethod(ic, "pinyin", true);
                frontend->call<ITestFrontend::pushCommitExpectation>("，");
                require(frontend->call<ITestFrontend::sendKeyEvent>(
                            uuid, Key(","), false),
                        "private custom punctuation leaked into ordinary "
                        "pinyin conversion");
                require(!ic->statusArea().isChild(prediction) &&
                            ic->statusArea().isChild(officialPrediction),
                        "private prediction action leaked into ordinary pinyin "
                        "status area");
                for (const auto &helper :
                     {"punctuation", "fullwidth", "chttrans"}) {
                    auto *privateAddon = instance.addonManager().addon(
                        std::string("chinese-llm-") + helper);
                    auto *officialAddon =
                        instance.addonManager().addon(helper, true);
                    auto *privateAction =
                        instance.userInterfaceManager().lookupAction(
                            std::string("chinese-llm-") + helper);
                    auto *officialAction =
                        instance.userInterfaceManager().lookupAction(helper);
                    require(privateAddon && officialAddon && privateAction &&
                                officialAction,
                            std::string(
                                "helper pair missing for stale action test: ") +
                                helper);
                    const auto privateConfig =
                        configSnapshot(privateAddon->getConfig());
                    const auto officialConfig =
                        configSnapshot(officialAddon->getConfig());
                    const auto privateText = privateAction->shortText(ic);
                    const auto officialText = officialAction->shortText(ic);
                    privateAction->activate(ic);
                    require(configSnapshot(privateAddon->getConfig()) ==
                                    privateConfig &&
                                configSnapshot(officialAddon->getConfig()) ==
                                    officialConfig &&
                                privateAction->shortText(ic) == privateText &&
                                officialAction->shortText(ic) == officialText,
                            std::string("stale private helper action mutated "
                                        "state in ordinary pinyin: ") +
                                helper);
                }
                ic->reset();
                instance.setCurrentInputMethod(ic, kLlmInputMethod, true);
                for (const auto &helper :
                     {"punctuation", "fullwidth", "chttrans"}) {
                    auto *action = instance.userInterfaceManager().lookupAction(
                        std::string("chinese-llm-") + helper);
                    require(action && ic->statusArea().isChild(action),
                            "private helper not restored after switch-back");
                    const auto before = action->shortText(ic);
                    action->activate(ic);
                    require(
                        action->shortText(ic) != before,
                        "private helper action failed on its own input method");
                    action->activate(ic);
                    require(action->shortText(ic) == before,
                            "private helper action failed to restore state");
                }
                finish(true, "");
                return;
            }
            if (scenario == "modifier-selection-global") {
                require(instance.inputMethodManager()
                                    .currentGroup()
                                    .inputMethodList()
                                    .size() == 3 &&
                            preeditText(ic).empty(),
                        "global shortcut probe requires three input methods "
                        "and empty composition");
                // Verify applicability through the public frontend path, not an
                // internal canEnumerate() helper: with no candidates, Shift
                // must enumerate. With candidates below, selection must take
                // precedence.
                frontend->call<ITestFrontend::sendKeyEvent>(
                    uuid, Key("Shift_L"), false);
                // Core filters global modifier releases without accepting
                // them; the actual input-method change is the success signal.
                frontend->call<ITestFrontend::sendKeyEvent>(
                    uuid, Key("Shift_L"), true);
                require(instance.inputMethod(ic) == "pinyin" ||
                            instance.inputMethod(ic) == "shuangpin",
                        "global shortcut probe did not enumerate away from "
                        "LLM拼音");
                ic->reset();
                instance.setCurrentInputMethod(ic, kLlmInputMethod, true);
                require(instance.inputMethod(ic) == kLlmInputMethod &&
                            preeditText(ic).empty(),
                        "global shortcut probe did not restore independent "
                        "LLM拼音");
            }
            if (scenario == "context" || scenario == "cache") {
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
            if (scenario == "virtual-keyboard") {
                for (char key : std::string(kKeys)) {
                    VirtualKeyboardEvent event(ic, false);
                    event.setKey(Key(std::string(1, key)));
                    instance.postEvent(event);
                    require(event.accepted(),
                            "LLM engine did not delegate virtual keyboard key");
                }
            } else {
                type(originalPinyin      ? "nihao"
                     : originalShuangpin ? "nihk"
                                         : kKeys);
            }
            generation = state()->generation;
            if (scenario == "disabled" || scenario == "dead") {
                require(
                    !preeditText(ic).empty() &&
                        hasCandidate(ic, "他们正在开会"),
                    "disabled/unavailable LLM lost native dictionary fallback");
            }
            if (original) {
                const auto list = ic->inputPanel().candidateList();
                require(list && !list->empty() && !preeditText(ic).empty(),
                        "official input method stopped producing dictionary "
                        "candidates/preedit");
                const auto beforeToggle = configSnapshot(llm->getConfig());
                const bool consumed =
                    frontend->call<ITestFrontend::sendKeyEvent>(
                        uuid, Key("Control+Alt+Shift+L"), false);
                require(!consumed &&
                            configSnapshot(llm->getConfig()) == beforeToggle,
                        "LLM toggle intercepted a key in the official input "
                        "method");
                require(state()->pinyin.empty() && state()->requestSeq == 0 &&
                            !state()->debounce,
                        "official input method scheduled an LLM request");
                if (originalPinyin) {
                    require(hasCandidate(ic, "你好"),
                            "ordinary pinyin lost its dictionary candidate");
                    frontend->call<ITestFrontend::pushCommitExpectation>(
                        "你好");
                    selectCandidate(ic, "你好");
                    require(preeditText(ic).empty(),
                            "ordinary pinyin dictionary commit did not reset");
                }
            }
            if (scenario == "preedit-click") {
                const auto beforeClick = preeditText(ic);
                const auto endCursor =
                    ic->inputPanel().clientPreedit().cursor();
                InvokeActionEvent click(InvokeActionEvent::Action::LeftClick, 0,
                                        ic);
                instance.postEvent(click);
                require(click.filtered() && preeditText(ic) == beforeClick,
                        "LLM engine did not preserve official pinyin "
                        "preedit-click behavior");
                require(
                    !ic->inputPanel().clientPreedit().empty() &&
                        ic->inputPanel().clientPreedit().cursor() == 0,
                    "preedit click did not move the official pinyin cursor");
                InvokeActionEvent restore(InvokeActionEvent::Action::LeftClick,
                                          endCursor, ic);
                instance.postEvent(restore);
                require(
                    restore.filtered() && preeditText(ic) == beforeClick &&
                        ic->inputPanel().clientPreedit().cursor() == endCursor,
                    "second preedit click did not restore composition cursor");
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
                        if (original) {
                            require(!appeared && state()->pinyin.empty() &&
                                        state()->requestSeq == 0 &&
                                        !state()->debounce,
                                    "official input method was affected by LLM "
                                    "state");
                            if (elapsed > std::chrono::milliseconds(1800)) {
                                require(mockRequestCount(endpoint) == 0,
                                        "official input method sent model "
                                        "requests");
                                require(
                                    configSnapshot(
                                        officialEngine->getConfigForInputMethod(
                                            *officialEntry)) ==
                                            officialConfigBefore &&
                                        configSnapshot(
                                            officialEngine
                                                ->getConfigForInputMethod(
                                                    *shuangpinEntry)) ==
                                            shuangpinConfigBefore,
                                    "official input method/toggle changed "
                                    "original settings");
                                finish(true, "");
                            }
                        } else if (scenario == "switch-inflight" ||
                                   scenario == "switch-results") {
                            if (stage == 0 &&
                                ((scenario == "switch-inflight" &&
                                  mockRequestCount(endpoint) >= 1) ||
                                 (scenario == "switch-results" && appeared))) {
                                require(scenario != "switch-inflight" ||
                                            state()->requestSeq != 0,
                                        "switch-out did not start with an "
                                        "in-flight request");
                                if (scenario == "switch-results") {
                                    retainedList =
                                        ic->inputPanel().candidateList();
                                    auto *common =
                                        dynamic_cast<CommonCandidateList *>(
                                            retainedList.get());
                                    require(common, "expected common list for "
                                                    "retained result test");
                                    // An immutable filtered view keeps the word
                                    // alive through invalidation, which
                                    // otherwise erases it from this same list.
                                    common->setFilter(
                                        [](const CandidateWord &candidate) {
                                            return candidate.text()
                                                       .toString() ==
                                                   kModelWord;
                                        });
                                    const auto *bulk = retainedList->toBulk();
                                    for (int i = 0; i < bulk->totalSize();
                                         ++i) {
                                        if (bulk->candidateFromAll(i)
                                                .text()
                                                .toString() == kModelWord) {
                                            retainedModelIndex = i;
                                            break;
                                        }
                                    }
                                    require(retainedModelIndex >= 0,
                                            "result-switch did not retain "
                                            "model candidate");
                                }
                                const auto oldGeneration = state()->generation;
                                instance.setCurrentInputMethod(ic, "pinyin",
                                                               true);
                                require(
                                    instance.inputMethod(ic) == "pinyin" &&
                                        state()->generation > oldGeneration &&
                                        state()->pinyin.empty() &&
                                        state()->candidates.empty() &&
                                        state()->requestSeq == 0 &&
                                        !state()->debounce,
                                    "switch-out did not invalidate LLM state");
                                type("nihao");
                                const auto beforeSelection = preeditText(ic);
                                if (retainedList) {
                                    // Keep a model word from the old panel
                                    // alive and attempt to select it after
                                    // switching to ordinary pinyin. It must not
                                    // commit.
                                    retainedList->toBulk()
                                        ->candidateFromAll(retainedModelIndex)
                                        .select(ic);
                                    require(preeditText(ic) ==
                                                    beforeSelection &&
                                                !beforeSelection.empty(),
                                            "retained model candidate "
                                            "committed in ordinary pinyin");
                                }
                                require(
                                    !hasCandidate(ic, kModelWord),
                                    "ordinary pinyin inherited LLM candidate");
                                stage = 1;
                                stageStart = Clock::now();
                            } else if (stage == 1) {
                                require(!appeared && state()->pinyin.empty() &&
                                            !state()->debounce,
                                        "late LLM reply landed after switching "
                                        "to ordinary pinyin");
                                if (stageElapsed >
                                    std::chrono::milliseconds(1200)) {
                                    require(mockRequestCount(endpoint) == 1,
                                            "ordinary pinyin submitted after "
                                            "LLM switch-out");
                                    ic->reset();
                                    instance.setCurrentInputMethod(
                                        ic, kLlmInputMethod, true);
                                    require(instance.inputMethod(ic) ==
                                                kLlmInputMethod,
                                            "failed to switch back to "
                                            "independent LLM拼音");
                                    type(kKeys);
                                    if (scenario == "switch-results") {
                                        require(hasCandidate(ic, kModelWord),
                                                "LLM result cache failed after "
                                                "switch-back");
                                    }
                                    stage = 2;
                                }
                            } else if (stage == 2 && appeared) {
                                checkCommit(kModelWord);
                                finish(true, "");
                            }
                        } else if (scenario == "disabled" ||
                                   scenario == "dead" ||
                                   scenario == "sensitive" ||
                                   scenario == "toggle") {
                            require(!appeared, "unexpected model candidate");
                            if (elapsed > std::chrono::milliseconds(1800)) {
                                require(!state()->debounce ||
                                            !state()->debounce->isEnabled(),
                                        "unexpected pending debounce");
                                if (scenario == "disabled" ||
                                    scenario == "dead") {
                                    require(hasCandidate(ic, "他们正在开会"),
                                            "native fallback vanished while "
                                            "waiting for LLM");
                                    checkCommit("他们正在开会");
                                }
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
                        } else if (modifierSelection && appeared) {
                            const auto list = ic->inputPanel().candidateList();
                            require(list && list->size() >= 2 &&
                                        list->candidate(1).text().toString() ==
                                            kModelWord,
                                    "modifier selection expected LLM word in "
                                    "second slot");
                            const auto beforePress = preeditText(ic);
                            // Keep the original synthetic zero-state case and
                            // also cover real frontends carrying Shift on
                            // release and a hardware keycode in the
                            // global-conflict case.
                            const bool realModifier =
                                scenario == "modifier-selection-global";
                            const Key press(FcitxKey_Shift_L, KeyStates{},
                                            realModifier ? 50 : 0);
                            const Key release(FcitxKey_Shift_L,
                                              realModifier
                                                  ? KeyStates(KeyState::Shift)
                                                  : KeyStates{},
                                              realModifier ? 50 : 0);
                            frontend->call<ITestFrontend::sendKeyEvent>(
                                uuid, press, false);
                            require(instance.inputMethod(ic) ==
                                            kLlmInputMethod &&
                                        preeditText(ic) == beforePress &&
                                        !beforePress.empty(),
                                    "modifier press was stolen by a global "
                                    "shortcut or committed before release");
                            frontend
                                ->call<ITestFrontend::pushCommitExpectation>(
                                    kModelWord);
                            require(frontend->call<ITestFrontend::sendKeyEvent>(
                                        uuid, release, true),
                                    "native modifier-only second-candidate "
                                    "release was not accepted");
                            require(instance.inputMethod(ic) ==
                                            kLlmInputMethod &&
                                        preeditText(ic).empty() &&
                                        state()->pinyin.empty(),
                                    "modifier-only second-candidate release "
                                    "switched input method or did not reset "
                                    "composition");
                            type("shui");
                            require(preeditText(ic) == "shui",
                                    "modifier selection left old native "
                                    "composition");
                            ic->reset();
                            finish(true, "");
                        } else if ((scenario == "cloud-badge" ||
                                    scenario == "badge-off") &&
                                   appeared) {
                            const bool badge = scenario == "cloud-badge";
                            auto checkBadge = [&]() {
                                auto list = ic->inputPanel().candidateList();
                                auto *bulk = list ? list->toBulk() : nullptr;
                                require(
                                    bulk,
                                    "LLM result has no bulk candidate list");
                                int matches = 0;
                                for (int i = 0; i < bulk->totalSize(); ++i) {
                                    const auto &word =
                                        bulk->candidateFromAll(i);
                                    if (word.text().toString() == kModelWord) {
                                        ++matches;
                                        require(
                                            word.comment().toString() ==
                                                (badge ? "\u2601\uFE0E" : ""),
                                            "cloud marker visibility is wrong");
                                        const auto displayed =
                                            instance
                                                .outputFilter(
                                                    ic,
                                                    word.textWithComment(
                                                        word.spaceBetweenComment()
                                                            ? " "
                                                            : ""))
                                                .toString();
                                        require(displayed ==
                                                    std::string(kModelWord) +
                                                        (badge ? " \u2601\uFE0E"
                                                               : ""),
                                                "Kimpanel candidate payload "
                                                "lost/duplicated cloud marker");
                                    }
                                    if (word.text().toString() ==
                                        "他们正在开会") {
                                        require(word.comment().toString().find(
                                                    "\u2601") ==
                                                    std::string::npos,
                                                "native dictionary candidate "
                                                "was falsely marked as LLM");
                                    }
                                }
                                require(matches == 1,
                                        "LLM UI refresh duplicated semantic "
                                        "candidate");
                                if (badge) {
                                    require(
                                        list->candidate(0).text().toString() ==
                                            kModelWord,
                                        "1-based CandidateIndex=1 did not put "
                                        "model result first");
                                }
                            };
                            checkBadge();
                            ic->updateUserInterface(
                                UserInterfaceComponent::InputPanel);
                            checkBadge();
                            if (badge) {
                                frontend->call<
                                    ITestFrontend::pushCommitExpectation>(
                                    kModelWord);
                                require(
                                    frontend->call<ITestFrontend::sendKeyEvent>(
                                        uuid, Key("space"), false),
                                    "first LLM candidate was not selected by "
                                    "Space");
                                require(preeditText(ic).empty() &&
                                            state()->pinyin.empty(),
                                        "cloud candidate commit left "
                                        "composition behind");
                            } else {
                                checkCommit(kModelWord);
                            }
                            finish(true, "");
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
