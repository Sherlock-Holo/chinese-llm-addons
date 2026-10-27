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
    {"mixed-badge-off", {"bang'wo'push'yi'xia", "帮我push一下"}},
    {"mixed-contiguous-surrounding", {"bangwocommityixia", "帮我commit一下"}},
    {"mixed-contiguous-surrounding-inflight",
     {"bangwocommityixia", "帮我commit一下"}},
    {"mixed-contiguous-surrounding-visible",
     {"bangwocommityixia", "帮我commit一下"}},
    {"mixed-context-changed", {"bangwocommityixia", "帮我commit一下"}},
    {"mixed-contiguous-commit", {"bangwocommityixia", "帮我commit一下"}},
    {"mixed-contiguous-commit-blocked",
     {"bangwocommityixia", "帮我commit一下"}},
    {"english", {"ying'wen", "Hello, world! C++ C#"}},
    {"digits", {"shu'zi", "2026"}},
    {"emoji",
     {"biao'qing",
      "😀 ☀\uFE0F ☁\uFE0E 👍🏽 👩\u200D💻 👨\u200D👩\u200D👧\u200D👦 "
      "🇨🇳 1\uFE0F\u20E3 #\uFE0F\u20E3 *\u20E3"}},
    {"emoji-tag",
     {"qi'zi", "\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E"
               "\U000E0067\U000E007F"}}};
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

nlohmann::json mockStats(const std::string &endpoint) {
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
    return nlohmann::json::parse(body);
}

size_t mockRequestCount(const std::string &endpoint) {
    return mockStats(endpoint).at("count").get<size_t>();
}

// Called only inside a main-thread callback after synchronous dispatch with
// DebounceMs=0. Worker start notifications are queued until that callback
// returns.
void waitForMockRequests(const std::string &endpoint, size_t expected) {
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < deadline) {
        if (mockRequestCount(endpoint) == expected) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("mock did not observe synchronous request start");
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
    const std::vector<std::string> scenarios{"live",
                                             "mixed",
                                             "mixed-badge-off",
                                             "mixed-contiguous-surrounding",
                                             "mixed-contiguous-surrounding-inflight",
                                             "mixed-contiguous-surrounding-visible",
                                             "mixed-context-changed",
                                             "mixed-contiguous-commit",
                                             "mixed-contiguous-commit-blocked",
                                             "english",
                                             "digits",
                                             "emoji",
                                             "emoji-tag",
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
                                             "llm-status",
                                             "stats-overlap",
                                             "stats-empty",
                                             "stats-error",
                                             "stats-invalid",
                                             "stats-all-filtered",
                                             "stats-limit",
                                             "stats-rate",
                                             "stats-focus",
                                             "history-menu",
                                             "history-clear-cycle",
                                             "history-clear-inflight",
                                             "history-disabled",
                                             "history-disable-live",
                                             "history-privacy",
                                             "history-stale",
                                             "history-native-blocked"};
    if (std::ranges::find(scenarios, scenario) == scenarios.end()) {
        std::cerr << "unknown scenario: " << scenario << '\n';
        return EXIT_FAILURE;
    }
    const auto payloadScenario = payloadScenarios.find(scenario);
    const bool payload = payloadScenario != payloadScenarios.end();
    const bool contiguousPayload = scenario.starts_with("mixed-contiguous-") ||
                                   scenario == "mixed-context-changed";
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
    fcitx::chinesellm::ChineseLlm *llmAddon = nullptr;
    AddonInstance *pinyin = nullptr;
    int statisticsStatusUpdates = 0;
    std::unique_ptr<HandlerTableEntry<EventHandler>> statisticsWatcher;
    std::unique_ptr<HandlerTableEntry<EventHandler>> focusWatcher;
    std::vector<std::string> historyPublishedLabels;
    std::vector<ScopedConnection> focusActionConnections;
    InputContext *dormantIc = nullptr;
    ICUUID dormantUuid{};
    int dormantStatusUpdates = 0;
    int activeStatusUpdates = 0;
    int dormantActionUpdates = 0;
    int activeActionUpdates = 0;
    int dormantStatusBefore = 0;
    int activeStatusBefore = 0;
    int dormantActionBefore = 0;
    int activeActionBefore = 0;
    ICUUID uuid{};
    Clock::time_point start;
    Clock::time_point stageStart;
    std::unique_ptr<EventSourceTime> poll;
    std::shared_ptr<CandidateList> retainedList;
    int retainedModelIndex = -1;
    std::string officialConfigBefore;
    std::string shuangpinConfigBefore;
    std::string viewedSnapshot;
    uint64_t contextChangedGeneration = 0;
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
                                            {"status", entry.status},
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
                {"live", {1, 1, 1}},
                {"mixed", {1, 1, 0}},
                {"mixed-badge-off", {1, 1, 0}},
                {"mixed-contiguous-surrounding", {1, 1, 0}},
                {"mixed-contiguous-surrounding-inflight", {1, 1, 0}},
                {"mixed-contiguous-surrounding-visible", {1, 1, 0}},
                {"mixed-context-changed", {1, 1, 0}},
                {"mixed-contiguous-commit", {1, 1, 0}},
                {"mixed-contiguous-commit-blocked", {1, 1, 0}},
                {"english", {1, 1, 0}},
                {"digits", {1, 1, 0}},
                {"emoji", {1, 1, 0}},
                {"emoji-tag", {1, 1, 0}},
                {"debounce", {1, 1, 1}},
                {"disabled", {0, 0, 0}},
                {"dead", {1, 0, 0}},
                {"preview", {1, 1, 1}},
                {"reset", {2, 1, 1}},
                {"partial", {1, 0, 0}},
                {"toggle", {0, 0, 0}},
                {"sensitive", {0, 0, 0}},
                {"toggle-inflight", {1, 0, 0}},
                {"sensitive-inflight", {1, 0, 0}},
                {"context", {2, 2, 0}},
                {"cache", {2, 2, 0}},
                {"cooldown", {2, 0, 0}},
                {"config", {2, 2, 2}},
                {"filtered", {1, 1, 1}},
                {"metadata", {0, 0, 0}},
                {"original-pinyin", {0, 0, 0}},
                {"original-shuangpin", {0, 0, 0}},
                {"switch-inflight", {2, 1, 1}},
                {"switch-results", {1, 1, 1}},
                {"own-config", {0, 0, 0}},
                {"virtual-keyboard", {1, 1, 1}},
                {"preedit-click", {1, 1, 1}},
                {"modifier-selection", {1, 1, 1}},
                {"modifier-selection-global", {1, 1, 1}},
                {"llm-alone", {1, 1, 1}},
                {"private-settings", {0, 0, 0}},
                {"customphrase-isolation", {0, 0, 0}},
                {"learning-isolation", {0, 0, 0}},
                {"learning-reload", {0, 0, 0}},
                {"private-actions", {0, 0, 0}},
                {"native-description", {0, 0, 0}},
                {"cloud-badge", {1, 1, 1}},
                {"badge-off", {1, 1, 1}},
                {"llm-status", {0, 0, 0}},
                {"stats-overlap", {1, 1, 1}},
                {"stats-empty", {1, 0, 0}},
                {"stats-error", {1, 0, 0}},
                {"stats-invalid", {1, 0, 0}},
                {"stats-all-filtered", {1, 0, 0}},
                {"stats-limit", {1, 1, 1}},
                {"stats-rate", {3, 3, 2}},
                {"stats-focus", {2, 0, 0}},
                {"history-menu", {1, 1, 1}},
                {"history-clear-cycle", {3, 3, 3}},
                {"history-clear-inflight", {2, 1, 1}},
                {"history-disabled", {1, 1, 1}},
                {"history-disable-live", {1, 1, 1}},
                {"history-privacy", {2, 1, 1}},
                {"history-stale", {1, 0, 0}},
                {"history-native-blocked", {1, 1, 1}}};
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
                    if (entry.status == status) {
                        return entry;
                    }
                }
                throw std::runtime_error("missing history status: " + status);
            };
            if (payload || scenario == "live" || scenario == "debounce" ||
                scenario == "badge-off" || scenario == "cloud-badge") {
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
            } else if (scenario == "stats-overlap" ||
                       scenario == "history-native-blocked") {
                const auto &entry = entryWithStatus("usable");
                require(entry.overlap == true && entry.adoption == "native" &&
                            entry.adoptedText == "你好",
                        "native equivalent candidate adoption was not recorded "
                        "separately from LLM selection");
            } else if (scenario == "stats-empty") {
                const auto &entry = entryWithStatus("empty");
                require(entry.started && entry.httpStatus == 200 &&
                            entry.content.empty() && entry.candidates.empty() &&
                            !entry.overlap.has_value(),
                        "empty completion history fields are wrong");
            } else if (scenario == "stats-error" ||
                       scenario == "stats-invalid" || scenario == "dead") {
                const auto &entry = entryWithStatus("error");
                require(entry.started && !entry.error.empty() &&
                            entry.candidates.empty() &&
                            !entry.overlap.has_value() &&
                            entry.httpStatus == (scenario == "stats-error"
                                                     ? 500
                                                     : scenario == "dead" ? 0
                                                                          : 200),
                        "error history lost transport/http/error information");
            } else if (scenario == "stats-all-filtered") {
                const auto &entry = entryWithStatus("filtered");
                require(entry.started && entry.httpStatus == 200 &&
                            !entry.content.empty() && entry.candidates.empty() &&
                            !entry.overlap.has_value(),
                        "filtered history lost raw completion or status");
                for (const auto *reason : {"key_echo", "han_length_mismatch",
                                          "invalid_text",
                                          "empty_or_punctuation"}) {
                    require(std::ranges::any_of(
                                entry.rejected,
                                [&](const auto &rejected) {
                                    return rejected.reason == reason &&
                                           !rejected.text.empty();
                                }),
                            std::string("filtered history lacks reason/text: ") +
                                reason);
                }
            } else if (scenario == "stats-limit") {
                const auto &entry = entryWithStatus("usable");
                require(entry.candidates ==
                            std::vector<std::string>{kModelWord,
                                                     "他们正在开会", kOtherWord} &&
                            entry.content.find("他们正在开会。") !=
                                std::string::npos &&
                            entry.overlap == true,
                        "history lost raw punctuation/full sanitized result "
                        "before display limit");
            } else if (scenario == "cache") {
                const auto &hit = entryWithStatus("cache_hit");
                require(entries.size() == 3 && &hit == &entries.back() &&
                            !hit.started && hit.httpStatus == 0 &&
                            hit.candidates == entries.front().candidates &&
                            counts.sent == 2 && counts.compared == 2,
                        "cache history was omitted or counted as a transfer");
            } else if (scenario == "reset" ||
                       scenario == "switch-inflight" ||
                       scenario == "toggle-inflight") {
                const auto &cancelled = entryWithStatus("cancelled");
                require(cancelled.started && cancelled.adoption.empty() &&
                            !cancelled.overlap.has_value(),
                        "cancelled history was lost, compared or adopted");
            } else if (scenario == "stats-focus") {
                require(std::ranges::any_of(entries, [](const auto &entry) {
                            return entry.status == "cancelled" &&
                                   entry.error == "focus_lost";
                        }),
                        "worker cancellation overwrote the exact focus_lost "
                        "history cause");
            } else if (scenario == "history-stale") {
                const auto &stale = entryWithStatus("stale");
                require(stale.started && stale.httpStatus == 200 &&
                            !stale.content.empty() && stale.adoption.empty() &&
                            !stale.overlap.has_value() &&
                            ic->propertyAs<LlmState>("chinese-llm")
                                ->candidates.empty(),
                        "stale result was dropped from history or exposed "
                        "to candidates/statistics");
            }
            if (scenario == "sensitive" ||
                scenario == "sensitive-inflight" ||
                scenario == "history-privacy" ||
                scenario == "history-disabled" ||
                scenario == "history-disable-live" ||
                scenario == "history-menu" || scenario == "history-clear-cycle" ||
                scenario == "history-clear-inflight" || original || counts.sent == 0) {
                require(entries.empty(),
                        "disabled/cleared/sensitive/official path retained "
                        "request history");
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
        focusActionConnections.clear();
        focusWatcher.reset();
        if (ic) {
            frontend->call<ITestFrontend::destroyInputContext>(uuid);
            if (ic == dormantIc) {
                dormantIc = nullptr;
            }
            ic = nullptr;
        }
        if (dormantIc) {
            frontend->call<ITestFrontend::destroyInputContext>(dormantUuid);
            dormantIc = nullptr;
        }
        instance.exit();
    };
    auto type = [&](const std::string &keys) {
        std::string prefix;
        for (char c : keys) {
            bool accepted;
            if (scenario == "mixed-contiguous-commit-blocked") {
                InputContextEventBlocker blocker(ic);
                accepted = frontend->call<ITestFrontend::sendKeyEvent>(
                    uuid, Key(std::string(1, c)), false);
            } else {
                accepted = frontend->call<ITestFrontend::sendKeyEvent>(
                    uuid, Key(std::string(1, c)), false);
            }
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
                                  scenario == "badge-off" ||
                                          scenario == "mixed-badge-off"
                                      ? "False"
                                      : "True");
            config.setValueByPath("CandidateIndex",
                                  scenario == "cloud-badge" ? "1" : "2");
            if (scenario == "stats-limit") {
                config.setValueByPath("MaxCandidates", "1");
                config.setValueByPath("ShowBadge", "False");
            }
            config.setValueByPath("Model", "mock");
            config.setValueByPath("ApiKey", scenario == "history-privacy"
                                                ? "history-private-key-sentinel"
                                                : "");
            config.setValueByPath("RecordHistory",
                                  scenario == "history-disabled" ? "False"
                                                                  : "True");
            config.setValueByPath("DebounceMs", "250");
            config.setValueByPath("TimeoutSeconds", "1");
            config.setValueByPath("PromptTemplate", "{context}\n{pinyin}");
            config.setValueByPath(
                "ContextLength",
                scenario == "history-privacy" ||
                        scenario == "mixed-contiguous-surrounding-visible" ||
                        scenario == "mixed-context-changed"
                    ? "64"
                    : scenario.starts_with("mixed-contiguous-surrounding")
                          ? "0"
                          : "2");
            config.setValueByPath("CacheSize",
                                  scenario == "cache" || scenario == "config" ||
                                          scenario == "switch-results" ||
                                          scenario == "history-menu" ||
                                          scenario == "history-clear-cycle"
                                      ? "256"
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
            if (scenario == "stats-focus") {
                RawConfig global;
                global.setValueByPath("Behavior/ShareInputState", "No");
                instance.globalConfig().load(global, true);
                require(instance.globalConfig().shareInputState() ==
                            PropertyPropagatePolicy::No,
                        "two-IC probe requires independent input method state");
                instance.inputContextManager().setPropertyPropagatePolicy(
                    PropertyPropagatePolicy::No);
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
            if (scenario == "history-clear-cycle" || scenario == "history-clear-inflight") {
                auto *history = instance.userInterfaceManager().lookupAction("chinese-llm-history");
                focusActionConnections.emplace_back(history->connect<Action::Update>(
                    [&, history](InputContext *target) {
                        if (target == ic) {
                            historyPublishedLabels.push_back(history->shortText(target));
                        }
                    }));
            }
            if (scenario == "stats-focus") {
                dormantIc = ic;
                dormantUuid = uuid;
                focusWatcher = instance.watchEvent(
                    EventType::InputContextUpdateUI, EventWatcherPhase::Default,
                    [&](Event &event) {
                        auto &update =
                            static_cast<InputContextUpdateUIEvent &>(event);
                        if (update.component() !=
                            UserInterfaceComponent::StatusArea) {
                            return;
                        }
                        if (update.inputContext() == dormantIc) {
                            ++dormantStatusUpdates;
                        } else if (update.inputContext() == ic) {
                            ++activeStatusUpdates;
                        }
                    });
                auto actions = statisticsActions();
                actions.push_back(instance.userInterfaceManager().lookupAction(
                    "chinese-llm-toggle"));
                for (auto *action : actions) {
                    focusActionConnections.emplace_back(
                        action->connect<Action::Update>(
                            [&](InputContext *target) {
                                if (target == dormantIc) {
                                    ++dormantActionUpdates;
                                } else if (target == ic) {
                                    ++activeActionUpdates;
                                }
                            }));
                }
            }
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
                clickStatistics();
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
                    checkStatistics(0, 0, 0);
                    clickStatistics();
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
            require(llmAddon->requestHistory().entries().empty() &&
                        llmAddon->historySnapshotPath().empty(),
                    "history must start empty and must not create a snapshot "
                    "before the user views it");
            if (scenario.starts_with("mixed-contiguous-surrounding") ||
                scenario == "mixed-context-changed") {
                const std::string context =
                    scenario == "mixed-contiguous-surrounding-visible" ||
                            scenario == "mixed-context-changed"
                        ? kContextBefore
                        : "safe-context";
                ic->surroundingText().setText(context, context.size(),
                                              context.size());
                ic->updateSurroundingText();
            }
            if (scenario == "history-privacy") {
                const std::string context = "private-context-never-record";
                ic->surroundingText().setText(context, context.size(),
                                              context.size());
                ic->updateSurroundingText();
            }
            if (scenario == "context" || scenario == "cache" ||
                scenario == "stats-rate") {
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
            } else if (payload) {
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
                if (scenario == "mixed-contiguous-surrounding" ||
                    scenario == "mixed-contiguous-surrounding-inflight") {
                    const bool inFlight =
                        scenario == "mixed-contiguous-surrounding-inflight";
                    if (inFlight) {
                        RawConfig immediate;
                        immediate.setValueByPath("DebounceMs", "0");
                        llm->setConfig(immediate);
                        ic->updateUserInterface(
                            UserInterfaceComponent::InputPanel);
                        waitForMockRequests(endpoint, 1);
                    }
                    const auto oldPinyin = state()->pinyin;
                    const auto oldContext = state()->context;
                    const auto oldGeneration = state()->generation;
                    const auto oldSeq = state()->requestSeq;
                    const auto *oldDebounce = state()->debounce.get();
                    const bool oldEnabled = oldDebounce &&
                                            oldDebounce->isEnabled();
                    require(oldContext.empty() &&
                                (inFlight ? oldSeq != 0
                                          : oldDebounce && oldEnabled),
                            "same-context probe must start with an in-flight "
                            "request/pending debounce and ContextLength=0");
                    // DBus frontends notify even if both surrounding text and
                    // cursor are unchanged. ContextLength=0 also guarantees the
                    // effective model context did not change.
                    ic->updateSurroundingText();
                    dumpComposition(ic, "same-surrounding-notification",
                                    payloadScenario->second.first, true);
                    require(
                        state()->pinyin == oldPinyin &&
                            state()->context == oldContext &&
                            state()->generation == oldGeneration &&
                            state()->requestSeq == oldSeq &&
                            state()->debounce.get() == oldDebounce &&
                            (!oldDebounce ||
                             state()->debounce->isEnabled() == oldEnabled),
                        "unchanged surrounding-text notification invalidated "
                        "the complete input/final debounce");
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
                type(originalPinyin || scenario == "stats-overlap" ||
                             scenario == "history-native-blocked" ? "nihao"
                     : originalShuangpin                           ? "nihk"
                                                                   : kKeys);
            }
            if (scenario == "stats-focus") {
                // Input was accumulated with normal debounce. Reconfigure and
                // explicitly rebuild once, dispatching only the complete input.
                RawConfig immediate;
                immediate.setValueByPath("DebounceMs", "0");
                llm->setConfig(immediate);
                ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                waitForMockRequests(endpoint, 1);
                require(llmAddon->requestStatistics().sent == 1,
                        "shell-focus probe did not start exactly one transfer");
                ic->focusOut();
                require(!ic->hasFocus() &&
                            instance.mostRecentInputContext() == ic &&
                            instance.inputMethod(ic) == kLlmInputMethod &&
                            ic->statusArea().isChild(statisticsActions()[0]),
                        "shell focus loss did not retain most-recent LLM menu");
                dormantStatusBefore = dormantStatusUpdates;
                dormantActionBefore = dormantActionUpdates;
            }
            generation = state()->generation;
            if (scenario == "history-stale") {
                RawConfig immediate;
                immediate.setValueByPath("DebounceMs", "0");
                llm->setConfig(immediate);
                ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                waitForMockRequests(endpoint, 1);
                // Deterministically expire only the composition version, without
                // cancelling the real HTTP transfer. This isolates the main-
                // thread stale guard; the real reset scenario tests cancellation.
                ++state()->generation;
            }
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
                        if (scenario == "history-native-blocked") {
                            const auto &entries =
                                llmAddon->requestHistory().entries();
                            if (stage == 0 &&
                                llmAddon->requestStatistics().compared == 1) {
                                const auto list =
                                    ic->inputPanel().candidateList();
                                require(list && !list->empty() &&
                                            list->candidate(0)
                                                    .text()
                                                    .toString() == "你好" &&
                                            list->candidate(0)
                                                .comment()
                                                .toString()
                                                .find("\u2601") ==
                                                std::string::npos &&
                                            entries.size() == 1 &&
                                            entries.back().adoption.empty(),
                                        "blocked adoption probe requires "
                                        "unadopted native first candidate");
                                frontend->call<
                                    ITestFrontend::pushCommitExpectation>(
                                    "你好");
                                {
                                    // Real frontends queue client-facing commits
                                    // until the key-event blocker is destroyed.
                                    InputContextEventBlocker blocker(ic);
                                    require(frontend->call<
                                                ITestFrontend::sendKeyEvent>(
                                                uuid, Key("space"), false),
                                            "native Space selection was not "
                                            "accepted under frontend blocker");
                                    require(preeditText(ic).empty() &&
                                                state()->pinyin.empty() &&
                                                state()->candidates.empty(),
                                            "native selection did not reset "
                                            "composition before queued commit");
                                    ic->updateUserInterface(
                                        UserInterfaceComponent::InputPanel);
                                }
                                stage = 1;
                            }
                            if (stage == 1 && !entries.empty() &&
                                entries.back().adoption == "native") {
                                require(entries.back().adoptedText == "你好",
                                        "queued native commit lost exact "
                                        "adopted text");
                                type("shui");
                                require(preeditText(ic) == "shui",
                                        "blocked native selection retained "
                                        "old composition");
                                ic->reset();
                                finish(true, "");
                            }
                        } else if (scenario == "history-stale") {
                            require(!appeared && state()->candidates.empty(),
                                    "version-expired result appeared in UI");
                            const auto &entries =
                                llmAddon->requestHistory().entries();
                            if (!entries.empty() &&
                                entries.back().status == "stale") {
                                finish(true, "");
                            }
                        } else if (scenario == "history-clear-inflight") {
                            auto *view = instance.userInterfaceManager().lookupAction("chinese-llm-history");
                            auto *clear = instance.userInterfaceManager().lookupAction("chinese-llm-history-clear");
                            if (stage == 0 && llmAddon->requestStatistics().sent == 1) {
                                require(!appeared && state()->requestSeq != 0 &&
                                            llmAddon->requestHistory().entries().size() == 1,
                                        "inflight-clear probe did not reach an unfinished network request");
                                generation = llmAddon->requestHistory().entries().back().id;
                                clear->activate(ic);
                                require(llmAddon->requestHistory().entries().empty() &&
                                            llmAddon->historySnapshotPath().empty(),
                                        "inflight clear retained the old row/snapshot");
                                // Reopen an empty page before the replacement
                                // input completes; it must later update in place.
                                view->activate(ic);
                                viewedSnapshot = llmAddon->historySnapshotPath();
                                checkPrivateSnapshot(viewedSnapshot);
                                ic->reset();
                                type("nihao");
                                stage = 1;
                            } else if (stage == 1 &&
                                       !llmAddon->requestHistory().entries().empty() &&
                                       llmAddon->requestHistory().entries().back().status == "usable" &&
                                       openerRecorded()) {
                                const auto &entries = llmAddon->requestHistory().entries();
                                require(entries.size() == 1 && entries.back().id > generation &&
                                            entries.back().content == "你好" &&
                                            entries.back().httpStatus == 200 && hasCandidate(ic, "你好"),
                                        "late old callbacks erased/resurrected rows or blocked new candidates");
                                const auto label = view->shortText(ic);
                                require((label == "LLM request history 1" || label == "LLM 请求历史 1") &&
                                            !historyPublishedLabels.empty() &&
                                            historyPublishedLabels.back() == label,
                                        "replacement request failed to publish the post-clear menu count");
                                checkStatistics(2, 1, 1);
                                const auto html = readTestData(viewedSnapshot);
                                require(html.find("你好") != std::string::npos &&
                                            html.find(kModelWord) == std::string::npos,
                                        "empty snapshot did not refresh or revived cleared response text");
                                view->activate(ic);
                                stage = 2;
                            } else if (stage == 2) {
                                std::istringstream lines(readTestData(openerLogPath()));
                                std::string line;
                                std::vector<nlohmann::json> launches;
                                while (std::getline(lines, line)) {
                                    auto record = nlohmann::json::parse(line, nullptr, false);
                                    if (record.is_object()) launches.push_back(std::move(record));
                                }
                                if (launches.size() == 2) {
                                    require(launches.back().at("path") == viewedSnapshot &&
                                                launches.back().at("html").get<std::string>().find("你好") !=
                                                    std::string::npos,
                                            "explicit post-clear reopening still displayed an empty page");
                                    clear->activate(ic);
                                    require(llmAddon->requestHistory().entries().empty() &&
                                                !std::filesystem::exists(viewedSnapshot),
                                            "final inflight probe clear retained data");
                                    ic->reset();
                                    finish(true, "");
                                }
                            }
                        } else if (scenario == "history-clear-cycle") {
                            auto *view = instance.userInterfaceManager().lookupAction(
                                "chinese-llm-history");
                            auto *clear = instance.userInterfaceManager().lookupAction(
                                "chinese-llm-history-clear");
                            auto checkCount = [&](size_t count) {
                                const auto label = view->shortText(ic);
                                require(llmAddon->requestHistory().entries().size() == count &&
                                            (label == "LLM request history " + std::to_string(count) ||
                                             label == "LLM 请求历史 " + std::to_string(count)),
                                        "history memory/menu count disagree after clear/new input: " + label);
                                require(!historyPublishedLabels.empty() &&
                                            historyPublishedLabels.back() == label,
                                        "history changed its label but did not publish a menu action update");
                            };
                            auto launches = [&] {
                                std::vector<nlohmann::json> records;
                                if (std::filesystem::exists(openerLogPath())) {
                                    std::istringstream lines(readTestData(openerLogPath()));
                                    std::string line;
                                    while (std::getline(lines, line)) {
                                        auto record = nlohmann::json::parse(line, nullptr, false);
                                        if (record.is_object()) records.push_back(std::move(record));
                                    }
                                }
                                return records;
                            };
                            if (stage == 0 && appeared) {
                                checkCount(1);
                                view->activate(ic);
                                viewedSnapshot = llmAddon->historySnapshotPath();
                                checkPrivateSnapshot(viewedSnapshot);
                                stage = 1;
                            } else if (stage == 1 && launches().size() == 1) {
                                const auto oldId = llmAddon->requestHistory().entries().back().id;
                                // Mirror a shell menu opening: its most-recent IC
                                // is temporarily unfocused, but owns the actions.
                                ic->focusOut();
                                require(instance.mostRecentInputContext() == ic,
                                        "history clear shell probe lost most-recent context");
                                clear->activate(ic);
                                checkCount(0);
                                require(llmAddon->historySnapshotPath().empty() &&
                                            !std::filesystem::exists(viewedSnapshot),
                                        "clear left the first browser snapshot alive");
                                checkStatistics(1, 1, 1);
                                require(configSnapshot(llm->getConfig()).find("RecordHistory=False") ==
                                            std::string::npos,
                                        "clear disabled recording");
                                generation = oldId;
                                ic->focusIn();
                                ic->reset();
                                type("ceshi"); // New uncached network request.
                                stage = 2;
                            } else if (stage == 2 &&
                                       !llmAddon->requestHistory().entries().empty() &&
                                       llmAddon->requestHistory().entries().back().status == "usable") {
                                checkCount(1);
                                const auto &entry = llmAddon->requestHistory().entries().back();
                                require(entry.id > generation && entry.started &&
                                            entry.content == "测试" && entry.httpStatus == 200,
                                        "fresh request after clear lost diagnostics or reused an ID");
                                checkStatistics(2, 2, 2);
                                frontend->call<ITestFrontend::pushCommitExpectation>("测试");
                                selectCandidate(ic, "测试");
                                type("ceshi"); // Same input, now a real cache hit.
                                checkCount(2);
                                require(llmAddon->requestHistory().entries().back().status == "cache_hit" &&
                                            !llmAddon->requestHistory().entries().back().started,
                                        "cache hit after clear did not create a new row");
                                view->activate(ic);
                                const auto newPath = llmAddon->historySnapshotPath();
                                require(newPath != viewedSnapshot && !newPath.empty(),
                                        "reopening after clear reused the deleted snapshot URL");
                                viewedSnapshot = newPath;
                                checkPrivateSnapshot(viewedSnapshot);
                                const auto html = readTestData(viewedSnapshot);
                                require(html.find("测试") != std::string::npos &&
                                            html.find(kModelWord) == std::string::npos,
                                        "reopened page is empty or resurrected cleared records");
                                stage = 3;
                            } else if (stage == 3 && launches().size() == 2) {
                                require(launches().back().at("path") == viewedSnapshot &&
                                            launches().back().at("html").get<std::string>().find("测试") !=
                                                std::string::npos,
                                        "actual reopened browser snapshot lacks new history");
                                clear->activate(ic);
                                checkCount(0);
                                require(!std::filesystem::exists(viewedSnapshot),
                                        "second clear retained its snapshot");
                                ic->reset();
                                type(kKeys); // Original cached keys must also resume history.
                                checkCount(1);
                                require(llmAddon->requestHistory().entries().back().status == "cache_hit" &&
                                            hasCandidate(ic, kModelWord),
                                        "second clear prevented logging an existing cached composition");
                                checkStatistics(2, 2, 2);
                                view->activate(ic);
                                const auto newPath = llmAddon->historySnapshotPath();
                                require(newPath != viewedSnapshot &&
                                            readTestData(newPath).find(kModelWord) != std::string::npos,
                                        "second reopened snapshot is empty");
                                viewedSnapshot = newPath;
                                stage = 4;
                            } else if (stage == 4 && launches().size() == 3) {
                                require(launches().back().at("path") == viewedSnapshot,
                                        "third explicit history launch used a stale URL");
                                const auto beforePinyin = state()->pinyin;
                                const auto beforeGeneration = state()->generation;
                                clear->activate(ic);
                                checkCount(0);
                                require(state()->pinyin == beforePinyin &&
                                            state()->generation == beforeGeneration,
                                        "clear should not reset the current composition");
                                // Keep typing the SAME composition, with no
                                // reset/commit/focus cycle to restore history.
                                type("ma");
                                stage = 5;
                            } else if (stage == 5 &&
                                       !llmAddon->requestHistory().entries().empty() &&
                                       llmAddon->requestHistory().entries().back().status == "usable") {
                                checkCount(1);
                                const auto &entry = llmAddon->requestHistory().entries().back();
                                require(entry.content == "他们正在开会吗" && entry.started &&
                                            entry.pinyin.ends_with("'ma"),
                                        "continuing the original composition after clear stopped logging");
                                checkStatistics(3, 3, 3);
                                view->activate(ic);
                                viewedSnapshot = llmAddon->historySnapshotPath();
                                require(readTestData(viewedSnapshot).find("他们正在开会吗") !=
                                            std::string::npos,
                                        "reopening after clear and continued typing gave an empty page");
                                stage = 6;
                            } else if (stage == 6 && launches().size() == 4) {
                                require(launches().back().at("path") == viewedSnapshot &&
                                            launches().back().at("html").get<std::string>().find("他们正在开会吗") !=
                                                std::string::npos,
                                        "fourth explicit history launch did not see the continued-input row");
                                clear->activate(ic);
                                checkCount(0);
                                ic->reset();
                                finish(true, "");
                            }
                        } else if (scenario == "history-menu") {
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
                                require(
                                    llmAddon->requestHistory()
                                                .entries()
                                                .back()
                                                .adoption == "llm" &&
                                        readTestData(viewedSnapshot) !=
                                            beforeAdoption,
                                    "adoption did not refresh already-viewed "
                                    "snapshot");
                                type(kKeys);
                                const auto &entries =
                                    llmAddon->requestHistory().entries();
                                require(hasCandidate(ic, kModelWord) &&
                                            entries.size() == 2 &&
                                            entries.back().status ==
                                                "cache_hit" &&
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
                        } else if (scenario == "history-disable-live") {
                            if (stage == 0 && appeared) {
                                require(
                                    !llmAddon->requestHistory().entries().empty(),
                                    "record-disable probe requires a completed "
                                    "history row");
                                instance.userInterfaceManager()
                                    .lookupAction("chinese-llm-history")
                                    ->activate(ic);
                                viewedSnapshot =
                                    llmAddon->historySnapshotPath();
                                checkPrivateSnapshot(viewedSnapshot);
                                stage = 1;
                            } else if (stage == 1 && openerRecorded()) {
                                const auto list =
                                    ic->inputPanel().candidateList();
                                const auto preedit = preeditText(ic);
                                const auto pinyin = state()->pinyin;
                                const auto candidates = state()->candidates;
                                const auto seq = state()->requestSeq;
                                const auto version = state()->generation;
                                RawConfig recording;
                                recording.setValueByPath("RecordHistory",
                                                         "False");
                                llm->setConfig(recording);
                                require(
                                    llmAddon->requestHistory().entries().empty() &&
                                        llmAddon->historySnapshotPath().empty() &&
                                        !std::filesystem::exists(
                                            viewedSnapshot) &&
                                        !std::filesystem::exists(
                                            std::filesystem::path(viewedSnapshot)
                                                .parent_path()) &&
                                        ic->inputPanel().candidateList() == list &&
                                        preeditText(ic) == preedit &&
                                        state()->pinyin == pinyin &&
                                        state()->candidates == candidates &&
                                        state()->requestSeq == seq &&
                                        state()->generation == version,
                                    "disabling recording retained private "
                                    "history or invalidated usable candidates");
                                checkStatistics(1, 1, 1);
                                checkCommit(kModelWord);
                                finish(true, "");
                            }
                        } else if (scenario == "history-privacy") {
                            if (stage == 0 && appeared) {
                                instance.userInterfaceManager()
                                .lookupAction("chinese-llm-history")
                                ->activate(ic);
                                viewedSnapshot =
                                    llmAddon->historySnapshotPath();
                                checkPrivateSnapshot(viewedSnapshot);
                                const auto html = readTestData(viewedSnapshot);
                                require(
                                    html.find("private-context-never-record") ==
                                            std::string::npos &&
                                        html.find("history-private-key-sentinel") ==
                                            std::string::npos &&
                                        html.find(kModelWord) !=
                                            std::string::npos,
                                    "history snapshot leaked context or "
                                    "credentials");
                                stage = 1;
                            } else if (stage == 1 && openerRecorded()) {
                                ic->reset();
                                type("ceshi");
                                stage = 2;
                            } else if (stage == 2 &&
                                       mockRequestCount(endpoint) >= 2) {
                                require(state()->requestSeq != 0,
                                        "privacy test did not enter second "
                                        "in-flight request");
                                ic->setCapabilityFlags(
                                    CapabilityFlag::PasswordOrSensitive);
                                require(
                                    llmAddon->requestHistory().entries().empty() &&
                                        llmAddon->historySnapshotPath().empty() &&
                                        !std::filesystem::exists(
                                            viewedSnapshot) &&
                                        !std::filesystem::exists(
                                            std::filesystem::path(
                                                viewedSnapshot)
                                                .parent_path()),
                                    "sensitive input did not immediately erase "
                                    "memory and viewed snapshot");
                                stage = 3;
                                stageStart = Clock::now();
                            } else if (stage == 3) {
                                require(
                                    llmAddon->requestHistory().entries().empty() &&
                                        llmAddon->historySnapshotPath().empty() &&
                                        !std::filesystem::exists(viewedSnapshot) &&
                                        state()->candidates.empty(),
                                    "late observation resurrected sensitive "
                                    "history/candidates/snapshot");
                                if (stageElapsed >
                                    std::chrono::milliseconds(1000)) {
                                    finish(true, "");
                                }
                            }
                        } else if (payload) {
                            const auto &expectedWord =
                                payloadScenario->second.second;
                            if (hasCandidate(ic, expectedWord)) {
                                if (scenario ==
                                    "mixed-contiguous-surrounding-visible") {
                                    require(state()->context == kContextBefore,
                                            "visible same-context probe did not "
                                            "use effective ContextLength=64");
                                    const auto oldPinyin = state()->pinyin;
                                    const auto oldContext = state()->context;
                                    const auto oldCandidates = state()->candidates;
                                    const auto oldPreedit = preeditText(ic);
                                    const auto oldGeneration = state()->generation;
                                    const auto oldSeq = state()->requestSeq;
                                    const auto oldList =
                                        ic->inputPanel().candidateList();
                                    const auto *oldDebounce =
                                        state()->debounce.get();
                                    const bool oldEnabled = oldDebounce &&
                                                            oldDebounce->isEnabled();
                                    ic->updateSurroundingText();
                                    dumpComposition(
                                        ic, "same-context-visible-notification",
                                        payloadScenario->second.first, true);
                                    require(
                                        state()->pinyin == oldPinyin &&
                                            state()->context == oldContext &&
                                            state()->candidates == oldCandidates &&
                                            preeditText(ic) == oldPreedit &&
                                            state()->generation == oldGeneration &&
                                            state()->requestSeq == oldSeq &&
                                            state()->debounce.get() == oldDebounce &&
                                            (!oldDebounce ||
                                             state()->debounce->isEnabled() ==
                                                 oldEnabled) &&
                                            ic->inputPanel().candidateList() ==
                                                oldList &&
                                            hasCandidate(ic, expectedWord),
                                        "same effective context erased already "
                                        "visible candidate or changed seq/gen/timer");
                                }
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
                                            entries.back().status == "usable" &&
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
                                if (scenario == "mixed-contiguous-surrounding-inflight") {
                                    // This probe used immediate dispatch. Restore
                                    // debounce before its unrelated post-commit
                                    // keys so they cannot race the final count.
                                    RawConfig restore;
                                    restore.setValueByPath("DebounceMs", "250");
                                    llm->setConfig(restore);
                                }
                                type("shui");
                                require(preeditText(ic) == "shui" &&
                                            !hasCandidate(ic, expectedWord),
                                        "payload commit retained old native "
                                        "composition/badge");
                                ic->reset();
                                finish(true, "");
                            }
                        } else if (scenario == "stats-focus") {
                            if (stage == 0 &&
                                dormantActionUpdates > dormantActionBefore) {
                                checkStatistics(1, 0, 0);
                                require(
                                    dormantStatusUpdates > dormantStatusBefore,
                                    "most-recent shell-unfocused IC did not "
                                    "receive queued statistics refresh");
                                uuid = frontend->call<
                                    ITestFrontend::createInputContext>(
                                    "testllmaddon-focus");
                                ic = instance.inputContextManager().findByUUID(
                                    uuid);
                                require(ic && ic != dormantIc,
                                        "two-IC probe failed to create B");
                                ic->focusIn();
                                instance.setCurrentInputMethod(ic, "pinyin",
                                                               true);
                                require(
                                    !dormantIc->hasFocus() && ic->hasFocus() &&
                                        instance.mostRecentInputContext() ==
                                            ic &&
                                        instance.inputMethod(dormantIc) ==
                                            kLlmInputMethod &&
                                        instance.inputMethod(ic) == "pinyin" &&
                                        dormantIc->statusArea().isChild(
                                            statisticsActions()[0]),
                                    "A must retain an unfocused LLM menu "
                                    "while independent B selects official");
                                checkStatistics(1, 0, 0);
                                dormantActionBefore = dormantActionUpdates;
                                dormantStatusBefore = dormantStatusUpdates;
                                RawConfig disabled;
                                disabled.setValueByPath("Enabled", "False");
                                llm->setConfig(disabled);
                                require(dormantActionUpdates ==
                                                dormantActionBefore &&
                                            dormantStatusUpdates ==
                                                dormantStatusBefore,
                                        "config status broadcast republished "
                                        "dormant A over B's official menu");
                                instance.setCurrentInputMethod(
                                    ic, kLlmInputMethod, true);
                                type(kKeys);
                                RawConfig enabled;
                                enabled.setValueByPath("Enabled", "True");
                                llm->setConfig(enabled);
                                ic->updateUserInterface(
                                    UserInterfaceComponent::InputPanel);
                                // Block this callback until the worker has
                                // queued started. Switch B before returning to
                                // the event loop; no dispatcher detach is used.
                                waitForMockRequests(endpoint, 2);
                                require(llmAddon->requestStatistics().sent == 2,
                                        "focus probe did not dispatch only one "
                                        "complete request per IC");
                                instance.setCurrentInputMethod(ic, "pinyin",
                                                               true);
                                require(instance.inputMethod(dormantIc) ==
                                                kLlmInputMethod &&
                                            !dormantIc->hasFocus() &&
                                            instance.mostRecentInputContext() ==
                                                ic &&
                                            dormantIc->statusArea().isChild(
                                                statisticsActions()[0]) &&
                                            !ic->statusArea().isChild(
                                                statisticsActions()[0]),
                                        "queued start probe lost retained A "
                                        "or active B's official isolation");
                                dormantActionBefore = dormantActionUpdates;
                                dormantStatusBefore = dormantStatusUpdates;
                                activeActionBefore = activeActionUpdates;
                                activeStatusBefore = activeStatusUpdates;
                                stage = 1;
                                stageStart = Clock::now();
                            } else if (stage == 1 &&
                                       stageElapsed >
                                           std::chrono::milliseconds(800)) {
                                // Global labels must advance, but their update
                                // must not republish either dormant LLM A or
                                // currently official B to Kimpanel.
                                checkStatistics(2, 0, 0);
                                require(
                                    dormantActionUpdates ==
                                            dormantActionBefore &&
                                        dormantStatusUpdates ==
                                            dormantStatusBefore &&
                                        activeActionUpdates ==
                                            activeActionBefore &&
                                        activeStatusUpdates ==
                                            activeStatusBefore,
                                    "queued start refreshed dormant A or "
                                    "official B instead of only global text");
                                clickStatistics();
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
                        } else if (scenario == "stats-empty" ||
                                   scenario == "stats-error" ||
                                   scenario == "stats-invalid" ||
                                   scenario == "stats-all-filtered") {
                            require(!appeared && state()->candidates.empty(),
                                    "unusable reply produced model candidates");
                            if (elapsed > std::chrono::milliseconds(1800)) {
                                checkStatistics(1, 0, 0);
                                require(hasCandidate(ic, "他们正在开会"),
                                        "unusable reply lost native fallback");
                                ic->updateUserInterface(
                                    UserInterfaceComponent::InputPanel);
                                clickStatistics();
                                checkStatistics(1, 0, 0);
                                finish(true, "");
                            }
                        } else if (scenario == "stats-limit" && appeared) {
                            checkStatistics(1, 1, 1);
                            require(
                                state()->candidates ==
                                        std::vector<std::string>{kModelWord,
                                                                 "他们正在开会",
                                                                 kOtherWord} &&
                                    !hasCandidate(ic, kOtherWord),
                                "MaxCandidates affected comparison input or "
                                "failed to limit displayed words");
                            ic->updateUserInterface(
                                UserInterfaceComponent::InputPanel);
                            clickStatistics();
                            checkStatistics(1, 1, 1);
                            finish(true, "");
                        } else if (scenario == "stats-rate") {
                            const auto counts = llmAddon->requestStatistics();
                            if (stage == 0 && counts.sent == 1 &&
                                counts.compared == 0 &&
                                statisticsStatusUpdates > 0) {
                                checkStatistics(1, 0, 0);
                                stage = 1;
                            }
                            if (stage <= 1 && appeared) {
                                require(stage == 1,
                                        "request start did not refresh "
                                        "statistics before its delayed reply");
                                checkStatistics(1, 1, 0);
                                require(statisticsStatusUpdates >= 2,
                                        "accepted reply did not refresh "
                                        "statistics status area");
                                ic->updateUserInterface(
                                    UserInterfaceComponent::InputPanel);
                                clickStatistics();
                                checkStatistics(1, 1, 0);
                                ic->reset();
                                llm->reloadConfig();
                                checkStatistics(1, 1, 0);
                                auto *toggle =
                                    instance.userInterfaceManager()
                                        .lookupAction("chinese-llm-toggle");
                                toggle->activate(ic);
                                checkStatistics(1, 1, 0);
                                toggle->activate(ic);
                                RawConfig change;
                                change.setValueByPath("MaxCandidates", "1");
                                change.setValueByPath("ShowBadge", "False");
                                llm->setConfig(change);
                                checkStatistics(1, 1, 0);
                                for (const auto *nativeEntry :
                                     {officialEntry, shuangpinEntry}) {
                                    instance.setCurrentInputMethod(
                                        ic, nativeEntry->uniqueName(), true);
                                    checkStatistics(1, 1, 0);
                                    clickStatistics();
                                }
                                instance.setCurrentInputMethod(
                                    ic, kLlmInputMethod, true);
                                checkStatistics(1, 1, 0);
                                ic->surroundingText().setText("", 0, 0);
                                ic->updateSurroundingText();
                                type(kKeys);
                                stage = 2;
                            } else if (stage == 2 && appeared) {
                                // The mixed reply has two candidates, but is
                                // exactly one overlapping request (50.0%).
                                checkStatistics(2, 2, 1);
                                ic->updateUserInterface(
                                    UserInterfaceComponent::InputPanel);
                                clickStatistics();
                                checkStatistics(2, 2, 1);
                                ic->reset();
                                type("nihao");
                                stage = 3;
                            } else if (stage == 3 && counts.compared == 3) {
                                // A third all-overlap request tests rounding
                                // (66.7%) and no visible LLM insertion.
                                checkStatistics(3, 3, 2);
                                clickStatistics();
                                finish(true, "");
                            }
                        } else if (original) {
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
