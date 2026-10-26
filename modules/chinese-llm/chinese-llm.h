/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef _CHINESE_LLM_CHINESE_LLM_H_
#define _CHINESE_LLM_CHINESE_LLM_H_

#include "llmbackend.h"
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
            "You are a Chinese pinyin input method. Convert the pinyin keys "
            "into the most likely Chinese sentence, using the context to "
            "disambiguate. Output only the Chinese characters on a single "
            "line. No pinyin, no explanation, no quotes.",
            {},
            {},
            {D_("chinese-llm",
                "Keep this short and unambiguous. The model must answer with "
                "Han characters only, otherwise its output is discarded.")}};
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

/// Per input context state. The candidate list is rebuilt from scratch on
/// every keystroke, so everything that must survive a rebuild lives here.
struct LlmState : public fcitx::InputContextProperty {
    /// Composition and context associated with the current request/results.
    std::string pinyin;
    std::string context;
    std::vector<std::string> candidates;
    uint64_t requestSeq = 0;
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

private:
    void handleUpdateUI(fcitx::Event &event);
    void handleKeyEvent(fcitx::Event &event);
    void invalidateState(fcitx::InputContext *ic, LlmState *state);
    void invalidateAll();
    void removeCandidates(fcitx::InputContext *ic, LlmState *state);
    std::string currentPinyin(fcitx::InputContext *ic) const;
    static std::string cacheKey(const std::string &pinyin,
                                const std::string &context);

    bool isLlmInputMethod(fcitx::InputContext *ic) const;
    /// Whether this input context is one we should generate candidates for.
    /// Not const: an expired cooldown is cleared as a side effect of asking.
    bool isEligible(fcitx::InputContext *ic);
    /// Recently committed text before the cursor, at most ContextLength chars.
    std::string buildContext(fcitx::InputContext *ic) const;
    /// Schedule a request for the given pinyin after the debounce delay.
    void scheduleRequest(fcitx::InputContext *ic, LlmState *state,
                         std::string pinyin, std::string context);
    /// Actually hand the request to the worker.
    void dispatchRequest(fcitx::InputContext *ic, LlmState *state,
                         std::string pinyin, std::string context,
                         uint64_t generation);
    /// Insert the given candidates at the configured position.
    void insertCandidates(fcitx::InputContext *ic, LlmState *state,
                          const std::vector<std::string> &candidates);
    void toggle();
    void refreshStatus();

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
    fcitx::EventDispatcher &dispatcher_;
    fcitx::FactoryFor<LlmState> factory_{
        [](fcitx::InputContext &) { return new LlmState; }};
    ChineseLlmConfig config_;
    fcitx::SimpleAction toggleAction_;
    fcitx::ScopedConnection toggleConnection_;
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
