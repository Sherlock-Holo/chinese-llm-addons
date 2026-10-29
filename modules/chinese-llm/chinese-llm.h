/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef _CHINESE_LLM_CHINESE_LLM_H_
#define _CHINESE_LLM_CHINESE_LLM_H_

#include "llmbackend.h"
#include "llmhistory.h"
#include "llmworker.h"
#include <chrono>
#include <cstdint>
#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/misc.h>
#include <fcitx-utils/signals.h>
#include <fcitx-utils/trackableobject.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fcitx::chinesellm {

FCITX_CONFIGURATION(
    ChineseLlmConfig,
    fcitx::SubConfigOption nativePinyin{
        this, "NativePinyin", D_("chinese-llm", "Independent Pinyin Settings"),
        "fcitx://config/addon/chinese-llm/pinyin"};
    fcitx::Option<bool> enabled{
        this, "Enabled", D_("chinese-llm", "Enable LLM candidates"), true};
    fcitx::OptionWithAnnotation<bool, fcitx::ToolTipAnnotation> recordHistory{
        this,
        "RecordHistory",
        D_("chinese-llm", "Record request history"),
        true,
        {},
        {},
        {D_("chinese-llm",
            "Keep the last 100 requests in memory, without context or "
            "credentials. Viewing history creates a private temporary "
            "snapshot. Disable to clear history.")}};
    fcitx::OptionWithAnnotation<std::string, fcitx::ToolTipAnnotation> endpoint{
        this,
        "Endpoint",
        D_("chinese-llm", "Endpoint"),
        "http://127.0.0.1:11434/v1/chat/completions",
        {},
        {},
        {D_("chinese-llm",
            "An OpenAI compatible chat completions URL. Works with ollama, "
            "llama.cpp's llama-server, and hosted compatible APIs.")}};
    fcitx::Option<std::string> model{this, "Model", D_("chinese-llm", "Model"),
                                     "qwen3:0.6b"};
    fcitx::OptionWithAnnotation<std::string, fcitx::ToolTipAnnotation> apiKey{
        this,
        "ApiKey",
        D_("chinese-llm", "API Key"),
        "",
        {},
        {},
        {D_("chinese-llm",
            "Sent as a bearer token. Leave empty for a local server that does "
            "not authenticate. Falls back to the environment variable "
            "FCITX_CHINESE_LLM_API_KEY when empty.")}};
    fcitx::Option<int, fcitx::IntConstrain> minPinyinLength{
        this, "MinPinyinLength", D_("chinese-llm", "Minimum Pinyin Length"), 4,
        fcitx::IntConstrain(1, 40)};
    fcitx::Option<int, fcitx::IntConstrain> debounceMs{
        this, "DebounceMs", D_("chinese-llm", "Debounce Delay (ms)"), 250,
        fcitx::IntConstrain(0, 2000)};
    fcitx::Option<int, fcitx::IntConstrain> timeoutSeconds{
        this, "TimeoutSeconds", D_("chinese-llm", "Request Timeout (seconds)"),
        5, fcitx::IntConstrain(1, 30)};
    fcitx::Option<int, fcitx::IntConstrain> maxCandidates{
        this, "MaxCandidates", D_("chinese-llm", "Max Candidates"), 3,
        fcitx::IntConstrain(1, 5)};
    fcitx::Option<int, fcitx::IntConstrain> candidateIndex{
        this, "CandidateIndex", D_("chinese-llm", "Candidate Position"), 2,
        fcitx::IntConstrain(1, 10)};
    fcitx::Option<int, fcitx::IntConstrain> contextLength{
        this, "ContextLength", D_("chinese-llm", "Context Length (characters)"),
        64, fcitx::IntConstrain(0, 256)};
    fcitx::Option<int, fcitx::IntConstrain> cacheSize{
        this, "CacheSize", D_("chinese-llm", "Cache Size"), 256,
        fcitx::IntConstrain(0, 4096)};
    fcitx::Option<int, fcitx::IntConstrain> maxConsecutiveErrors{
        this, "MaxConsecutiveErrors",
        D_("chinese-llm", "Errors Before Cooldown"), 5,
        fcitx::IntConstrain(1, 50)};
    fcitx::Option<int, fcitx::IntConstrain> cooldownSeconds{
        this, "CooldownSeconds", D_("chinese-llm", "Cooldown (seconds)"), 120,
        fcitx::IntConstrain(10, 3600)};
    fcitx::OptionWithAnnotation<bool, fcitx::ToolTipAnnotation> showBadge{
        this,
        "ShowBadge",
        D_("chinese-llm", "Show cloud marker on LLM candidates"),
        true,
        {},
        {},
        {D_("chinese-llm",
            "Display a cloud symbol beside LLM-generated candidates. "
            "The symbol is not committed as text. Dictionary "
            "candidates are not marked, including duplicate model results.")}};
    fcitx::Option<std::string, BadgeSymbolConstrain,
                  fcitx::DefaultMarshaller<std::string>,
                  fcitx::ToolTipAnnotation>
        badgeSymbol{
            this,
            "BadgeSymbol",
            D_("chinese-llm", "Symbol for LLM candidates"),
            "\u2601\uFE0E",
            {},
            {},
            {D_("chinese-llm",
                "Up to four characters, shown beside candidates generated by "
                "the LLM. Leave it empty to restore the default. A value that "
                "is longer, or contains a colon or a control character, is "
                "rejected and the previous symbol is kept.")}};
    fcitx::OptionWithAnnotation<bool, fcitx::ToolTipAnnotation>
        showOverlapBadge{
            this,
            "ShowOverlapBadge",
            D_("chinese-llm", "Mark candidates the LLM agrees with"),
            true,
            {},
            {},
            {D_("chinese-llm",
                "Display a gear (⚙) beside a dictionary candidate when the "
                "LLM produced the same text. The symbol is not committed as "
                "text.")}};
    fcitx::Option<std::string, BadgeSymbolConstrain,
                  fcitx::DefaultMarshaller<std::string>,
                  fcitx::ToolTipAnnotation>
        overlapSymbol{
            this,
            "OverlapSymbol",
            D_("chinese-llm", "Symbol for agreeing candidates"),
            "\u2699\uFE0E",
            {},
            {},
            {D_("chinese-llm",
                "Up to four characters, shown beside a dictionary candidate "
                "that the LLM also produced. Leave it empty to restore the "
                "default. A value that is longer, or contains a colon or a "
                "control character, is rejected and the previous symbol is "
                "kept.")}};
    fcitx::Option<fcitx::KeyList> toggleKey{
        this,
        "ToggleKey",
        D_("chinese-llm", "Toggle Key"),
        {fcitx::Key("Control+Alt+Shift+L")}};
    fcitx::OptionWithAnnotation<std::string, fcitx::ToolTipAnnotation>
        systemPrompt{
            this,
            "SystemPrompt",
            D_("chinese-llm", "System Prompt"),
            "You are a Chinese input method. Convert the typed keys into the "
            "most likely text, using the context to disambiguate. Preserve "
            "English words, numbers and emoji when appropriate. Output only "
            "candidate text on a single line. No echoed keys, no explanation, "
            "no quotes.",
            {},
            {},
            {D_("chinese-llm",
                "Keep this short and unambiguous. Output only candidate text "
                "on a single line, without explanations or echoed keys. "
                "Chinese, English, digits and emoji are supported.")}};
    fcitx::OptionWithAnnotation<std::string, fcitx::ToolTipAnnotation>
        promptTemplate{this,
                       "PromptTemplate",
                       D_("chinese-llm", "Prompt Template"),
                       "{context}\n{pinyin}",
                       {},
                       {},
                       {D_("chinese-llm",
                           "{context} is replaced with the recently committed "
                           "text, {pinyin} with the keys being typed.")}};);

/// The composition a request, a reply or a displayed candidate belongs to. One
/// value answers "is this still what the user is typing?" everywhere.
struct InputSnapshot {
    uint64_t generation = 0;
    std::string pinyin;
    std::string context;
    /// Hanzi the user already accepted in this composition. They are not in the
    /// client's surrounding text yet, and they must be committed in front of
    /// whatever the model candidate says.
    std::string selected;
};

/// What the native engine is composing right now, split at the point where the
/// user stopped accepting words.
struct Composition {
    /// Unselected pinyin up to the cursor: the part the native candidates are
    /// for, and therefore the part the model is asked about. Empty when this
    /// is not plain pinyin.
    std::string pinyin;
    /// Hanzi already accepted from the front of the composition.
    std::string selected;
};

/// Per input context state. The candidate list is rebuilt from scratch on
/// every keystroke, so everything that must survive a rebuild lives here.
struct LlmState : public fcitx::InputContextProperty {
    /// Composition and context associated with the current request/results.
    std::string pinyin;
    std::string context;
    std::string selected;
    std::vector<std::string> candidates;
    uint64_t requestSeq = 0;
    uint64_t historyId = 0;
    // Native commits may be delivered after the engine's composition reset.
    uint64_t commitHistoryId = 0;
    /// The accepted hanzi that belong to commitHistoryId's commit.
    std::string commitSelected;
    /// Pending debounce timer, reset on every keystroke.
    std::unique_ptr<fcitx::EventSourceTime> debounce;
    /// Guards against re-entering our own UI update. Inserting a candidate and
    /// refreshing the panel emits InputContextUpdateUI again; without this flag
    /// that would recurse forever.
    bool inserting = false;
    /// Bumped whenever the pinyin changes, so a reply for an older pinyin can
    /// be recognised as stale even after the state is updated.
    uint64_t generation = 0;
};

/// Session-only aggregates. A comparable request is a current, accepted network
/// reply with at least one usable candidate. Overlap is any exact native match,
/// counted once per request, not once per word. No input content is retained.
struct RequestStatistics {
    uint64_t sent = 0;
    uint64_t compared = 0;
    uint64_t overlapping = 0;
};

// A separate input method with a privately compiled native pinyin engine.
// Both native settings/learning and LLM configuration belong to this addon.
class ChineseLlm : public fcitx::InputMethodEngineV4,
                   public fcitx::TrackableObject<ChineseLlm> {
public:
    ChineseLlm(fcitx::AddonManager *manager);
    ~ChineseLlm() override;

    void reloadConfig() override;
    const fcitx::Configuration *getConfig() const override { return &config_; }
    void setConfig(const fcitx::RawConfig &config) override;
    void save() override;
    const fcitx::Configuration *
    getSubConfig(const std::string &path) const override;
    void setSubConfig(const std::string &path,
                      const fcitx::RawConfig &config) override;

    // Engine events are forwarded to our owned native engine, never to the
    // distribution's pinyin addon.
    void keyEvent(const fcitx::InputMethodEntry &entry,
                  fcitx::KeyEvent &event) override;
    void activate(const fcitx::InputMethodEntry &entry,
                  fcitx::InputContextEvent &event) override;
    void deactivate(const fcitx::InputMethodEntry &entry,
                    fcitx::InputContextEvent &event) override;
    void reset(const fcitx::InputMethodEntry &entry,
               fcitx::InputContextEvent &event) override;
    void filterKey(const fcitx::InputMethodEntry &entry,
                   fcitx::KeyEvent &event) override;
    std::string subMode(const fcitx::InputMethodEntry &entry,
                        fcitx::InputContext &ic) override;
    std::string subModeIconImpl(const fcitx::InputMethodEntry &entry,
                                fcitx::InputContext &ic) override;
    std::string subModeLabelImpl(const fcitx::InputMethodEntry &entry,
                                 fcitx::InputContext &ic) override;
    void invokeActionImpl(const fcitx::InputMethodEntry &entry,
                          fcitx::InvokeActionEvent &event) override;
    void virtualKeyboardEventImpl(const fcitx::InputMethodEntry &entry,
                                  fcitx::VirtualKeyboardEvent &event) override;

    /// Exposed for tests.
    LlmState *stateFor(fcitx::InputContext *ic) {
        return ic->propertyFor(&factory_);
    }
    /// Main-thread snapshot; sent includes failed/cancelled network attempts.
    RequestStatistics requestStatistics() const {
        return {worker_->requestsStarted(), comparedRequests_,
                overlappingRequests_};
    }
    const RequestHistory &requestHistory() const { return history_; }
    std::string historySnapshotPath() const { return historySnapshot_.path(); }
    // launchBrowser=false lets isolated tests inspect the same snapshot safely.
    void openHistory(bool launchBrowser = true);
    void clearHistory();

private:
    void handleUpdateUI(fcitx::Event &event);
    void handleKeyEvent(fcitx::Event &event);
    void invalidateState(fcitx::InputContext *ic, LlmState *state,
                         bool preserveCommit = false,
                         std::string_view reason = "composition_invalidated");
    void invalidateAll();
    void removeCandidates(fcitx::InputContext *ic, LlmState *state);
    /// Drop invalid marker symbols from an incoming configuration, so the
    /// previous value survives, and tell the user why.
    void rejectInvalidSymbols(fcitx::RawConfig &config);
    void notifyUser(const std::string &tipId, const std::string &summary,
                    const std::string &body);
    /// Whether snapshot still describes the focused, eligible composition.
    bool matchesInput(fcitx::InputContext *ic, const InputSnapshot &snapshot);
    void handleFetch(fcitx::InputContext *ic, const InputSnapshot &snapshot,
                     uint64_t historyId, FetchStage stage,
                     const FetchResult &result);
    /// Reads the private native engine's composition directly. The preedit is
    /// no use for this: after a partial selection it is hanzi and pinyin mixed.
    Composition currentComposition(fcitx::InputContext *ic) const;
    static std::string cacheKey(const std::string &pinyin,
                                const std::string &context);

    bool isLlmInputMethod(fcitx::InputContext *ic) const;
    /// Whether this input context is one we should generate candidates for.
    /// Not const: an expired cooldown is cleared as a side effect of asking.
    bool isEligible(fcitx::InputContext *ic);
    /// Recently committed text before the cursor followed by the hanzi already
    /// accepted in this composition, at most ContextLength chars.
    std::string buildContext(fcitx::InputContext *ic,
                             const std::string &selected) const;
    /// Schedule a request for the given pinyin after the debounce delay.
    void scheduleRequest(fcitx::InputContext *ic, LlmState *state,
                         std::string pinyin, std::string context,
                         std::string selected);
    /// Actually hand the request to the worker.
    void dispatchRequest(fcitx::InputContext *ic, LlmState *state,
                         std::string pinyin, std::string context,
                         std::string selected, uint64_t generation);
    /// Insert the given candidates at the configured position.
    void insertCandidates(fcitx::InputContext *ic, LlmState *state,
                          const std::vector<std::string> &candidates);
    void toggle();
    void refreshStatus();
    void refreshStatistics();
    /// Update the menu entry now and coalesce snapshot rewrites.
    void refreshHistory();
    bool writeHistorySnapshot();
    void scheduleHistorySnapshot();
    static bool overlapsNative(fcitx::InputContext *ic,
                               const std::vector<std::string> &candidates);
    void compareNativeCandidates(fcitx::InputContext *ic,
                                 const std::vector<std::string> &candidates);

    void noteError(const std::string &error);
    bool inCooldown() const;
    /// Clear the cooldown state once it has expired.
    void expireCooldown();
    void rememberCandidates(const std::string &key,
                            const std::vector<std::string> &candidates);

    BackendConfig backendConfig() const;

    // Owned by this addon, with independent config, dictionaries and history.
    // The official pinyin addon can be absent or disabled.
    std::unique_ptr<fcitx::InputMethodEngine> pinyin_;
    const fcitx::InputMethodEntry pinyinEntry_{"llm-pinyin", "LLM拼音", "zh_CN",
                                               "chinese-llm"};
    std::unique_ptr<LlmWorker> worker_;
    fcitx::Instance *instance_;
    FCITX_ADDON_DEPENDENCY_LOADER(notifications, instance_->addonManager());
    fcitx::EventDispatcher &dispatcher_;
    fcitx::FactoryFor<LlmState> factory_{
        [](fcitx::InputContext &) { return new LlmState; }};
    ChineseLlmConfig config_;
    fcitx::SimpleAction toggleAction_;
    // Read-only menu rows. No activation callbacks or configuration bindings.
    fcitx::SimpleAction requestsAction_;
    fcitx::SimpleAction overlapAction_;
    fcitx::SimpleAction rateAction_;
    fcitx::ScopedConnection toggleConnection_;
    fcitx::SimpleAction historyAction_;
    fcitx::SimpleAction clearHistoryAction_;
    fcitx::ScopedConnection historyConnection_;
    fcitx::ScopedConnection clearHistoryConnection_;
    RequestHistory history_;
    HistorySnapshot historySnapshot_;
    /// Pending coalesced rewrite of an already opened snapshot.
    std::unique_ptr<fcitx::EventSourceTime> historySnapshotTimer_;
    bool historySnapshotDirty_ = false;
    uint64_t comparedRequests_ = 0;
    uint64_t overlappingRequests_ = 0;
    /// What the overlap mark on screen currently looks like, so a symbol changed
    /// in the settings can be taken back before the new one is written.
    std::string appliedOverlapSymbol_;
    std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>>
        eventWatchers_;

    /// LRU cache keyed by pinyin and preceding text. Configuration changes
    /// clear it so results from different models/prompts never mix.
    std::unordered_map<std::string, std::vector<std::string>> cache_;
    std::vector<std::string> cacheOrder_;

    int errorCount_ = 0;
    std::chrono::steady_clock::time_point cooldownUntil_{};
};

class ChineseLlmFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        // Bind before constructing either native or LLM configuration labels.
        fcitx::registerDomain("chinese-llm", FCITX_INSTALL_LOCALEDIR);
        return new ChineseLlm(manager);
    }
};

} // namespace fcitx::chinesellm

#endif // _CHINESE_LLM_CHINESE_LLM_H_
