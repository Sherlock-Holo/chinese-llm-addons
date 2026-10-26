/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "chinese-llm.h"
#include "llmbackend.h"
#include "llmworker.h"
#include "native/pinyin/pinyin.h"
#include <algorithm>
#include <cstdlib>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fcitx::chinesellm {

namespace {

/// The one definition of the log category. llmworker.cpp only declares it, so
/// the same category name is used without a duplicate definition.
FCITX_DEFINE_LOG_CATEGORY(chinesellm, "chinese-llm");

constexpr char kEnvApiKey[] = "FCITX_CHINESE_LLM_API_KEY";
/// Kimpanel flattens textWithComment() into the candidate's display string.
/// Keep semantic text untouched for deduplication and commit, and request a
/// monochrome cloud (U+2601 U+FE0E), not a colored emoji, in its display
/// comment.
constexpr char kBadge[] = "\u2601\uFE0E";

/**
 * A candidate backed by model output.
 *
 * Selecting it commits the text, clears the input panel, and resets the input
 * context. The reset is what makes the pinyin engine drop its composition:
 * Instance watches InputContextReset in the InputMethod phase and calls
 * PinyinEngine::reset(), which clears the buffer and rebuilds the panel.
 * Without it the engine would keep the half-typed pinyin after our text went
 * out.
 */
class LlmCandidateWord : public fcitx::CandidateWord {
public:
    LlmCandidateWord(std::string text, bool badge,
                     std::function<bool(fcitx::InputContext *)> valid)
        : text_(std::move(text)), valid_(std::move(valid)) {
        setText(fcitx::Text(text_));
        if (badge) {
            fcitx::Text comment(kBadge);
            setComment(comment);
            setSpaceBetweenComment(true);
        }
    }

    void select(fcitx::InputContext *inputContext) const override {
        if (!valid_(inputContext)) {
            return;
        }
        inputContext->commitString(text_);
        inputContext->inputPanel().reset();
        inputContext->reset();
    }

private:
    std::string text_;
    std::function<bool(fcitx::InputContext *)> valid_;
};

} // namespace

ChineseLlm::ChineseLlm(fcitx::AddonManager *manager)
    : pinyin_(std::make_unique<fcitx::llmnative::PinyinEngine>(
          manager->instance())),
      worker_(std::make_unique<LlmWorker>()), instance_(manager->instance()),
      dispatcher_(manager->instance()->eventDispatcher()) {
    instance_->inputContextManager().registerProperty("chinese-llm", &factory_);

    toggleAction_.setCheckable(true);
    toggleAction_.registerAction("chinese-llm-toggle",
                                 &instance_->userInterfaceManager());
    toggleConnection_ = toggleAction_.connect<SimpleAction::Activated>(
        [this](InputContext *ic) {
            // Kimpanel targets the most recent IC; opening its shell menu may
            // temporarily move keyboard focus away. Gate ownership, not focus.
            // Ignore stale menu clicks after switching away from our entry.
            if (ic && isLlmInputMethod(ic) &&
                ic->statusArea().isChild(&toggleAction_)) {
                toggle();
            }
        });
    reloadConfig();

    // Fired by the pinyin engine after it has rebuilt the candidate list, so
    // the list is guaranteed to exist and be complete when this runs.
    eventWatchers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextUpdateUI,
        fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) { handleUpdateUI(event); }));

    // PostInputMethod so the engine has already consumed the key and, in
    // particular, so our toggle key does not steal a key the engine wants.
    eventWatchers_.emplace_back(instance_->watchEvent(
        fcitx::EventType::InputContextKeyEvent,
        fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) { handleKeyEvent(event); }));

    // Composition boundaries invalidate replies even if the next input is
    // identical. Do this before the engine clears or changes its own state.
    for (const auto type :
         {EventType::InputContextReset, EventType::InputContextFocusOut,
          EventType::InputContextSwitchInputMethod,
          EventType::InputContextCapabilityAboutToChange,
          EventType::InputContextSurroundingTextUpdated}) {
        eventWatchers_.emplace_back(instance_->watchEvent(
            type, EventWatcherPhase::PreInputMethod, [this](Event &event) {
                auto *ic =
                    static_cast<InputContextEvent &>(event).inputContext();
                invalidateState(ic, ic->propertyFor(&factory_));
            }));
    }

    worker_->start();
    FCITX_INFO() << "chinese-llm: loaded, endpoint "
                 << config_.endpoint.value();
}

ChineseLlm::~ChineseLlm() {
    toggleConnection_.disconnect();
    eventWatchers_.clear();
    invalidateAll();
    worker_->stop();
}

void ChineseLlm::keyEvent(const InputMethodEntry &, KeyEvent &event) {
    pinyin_->keyEvent(pinyinEntry_, event);
}

void ChineseLlm::activate(const InputMethodEntry &, InputContextEvent &event) {
    auto *ic = event.inputContext();
    invalidateState(ic, ic->propertyFor(&factory_));
    pinyin_->activate(pinyinEntry_, event);
    ic->statusArea().addAction(StatusGroup::InputMethod, &toggleAction_);
    toggleAction_.update(ic);
}

void ChineseLlm::deactivate(const InputMethodEntry &,
                            InputContextEvent &event) {
    auto *ic = event.inputContext();
    // Remove AI candidates before native deactivation handles commit/clear.
    invalidateState(ic, ic->propertyFor(&factory_));
    pinyin_->deactivate(pinyinEntry_, event);
    // Retain the selected IM's action across transient focus loss (shell
    // menus). Fcitx clears the InputMethod status group before activating
    // another IM.
}

void ChineseLlm::reset(const InputMethodEntry &, InputContextEvent &event) {
    auto *ic = event.inputContext();
    invalidateState(ic, ic->propertyFor(&factory_));
    pinyin_->reset(pinyinEntry_, event);
}

void ChineseLlm::filterKey(const InputMethodEntry &, KeyEvent &event) {
    pinyin_->filterKey(pinyinEntry_, event);
}

std::string ChineseLlm::subMode(const InputMethodEntry &, InputContext &ic) {
    return pinyin_->subMode(pinyinEntry_, ic);
}

std::string ChineseLlm::subModeIconImpl(const InputMethodEntry &,
                                        InputContext &ic) {
    return pinyin_->subModeIcon(pinyinEntry_, ic);
}

std::string ChineseLlm::subModeLabelImpl(const InputMethodEntry &,
                                         InputContext &) {
    // GNOME Kimpanel truncates ASCII labels to two characters.
    return config_.enabled.value() ? "AI" : "PY";
}

void ChineseLlm::invokeActionImpl(const InputMethodEntry &,
                                  InvokeActionEvent &event) {
    pinyin_->invokeAction(pinyinEntry_, event);
}

void ChineseLlm::virtualKeyboardEventImpl(const InputMethodEntry &,
                                          VirtualKeyboardEvent &event) {
    pinyin_->virtualKeyboardEvent(pinyinEntry_, event);
}

void ChineseLlm::reloadConfig() {
    readAsIni(config_, "conf/chinese-llm.conf");
    invalidateAll();
    pinyin_->reloadConfig();
    cache_.clear();
    cacheOrder_.clear();
    errorCount_ = 0;
    cooldownUntil_ = {};
    refreshStatus();
}

void ChineseLlm::setConfig(const fcitx::RawConfig &config) {
    config_.load(config, true);
    fcitx::safeSaveAsIni(config_, "conf/chinese-llm.conf");
    invalidateAll();
    cache_.clear();
    cacheOrder_.clear();
    errorCount_ = 0;
    cooldownUntil_ = {};
    refreshStatus();
}

void ChineseLlm::save() {
    fcitx::safeSaveAsIni(config_, "conf/chinese-llm.conf");
    pinyin_->save();
}

const Configuration *ChineseLlm::getSubConfig(const std::string &path) const {
    if (path == "pinyin") {
        // The private engine is full-pinyin-only and already hides unsupported
        // options. Return its live configuration, not upstream's cached
        // pyConfig copy, so toolbar prediction toggles are reflected in this
        // GUI page.
        return pinyin_->getConfig();
    }
    return nullptr;
}

void ChineseLlm::setSubConfig(const std::string &path,
                              const RawConfig &config) {
    // All native setters/commands target the owned private engine. Never
    // forward a config URL or a command to the installed pinyin addon.
    if (path == "pinyin") {
        invalidateAll();
        pinyin_->setConfigForInputMethod(pinyinEntry_, config);
    } else if (path.starts_with("pinyin/")) {
        invalidateAll();
        pinyin_->setSubConfig(path.substr(7), config);
    }
}

BackendConfig ChineseLlm::backendConfig() const {
    BackendConfig backend;
    backend.endpoint = config_.endpoint.value();
    backend.model = config_.model.value();
    backend.apiKey = config_.apiKey.value();
    if (backend.apiKey.empty()) {
        if (const char *env = std::getenv(kEnvApiKey); env && *env) {
            backend.apiKey = env;
        }
    }
    backend.systemPrompt = config_.systemPrompt.value();
    backend.promptTemplate = config_.promptTemplate.value();
    backend.maxTokens = 64;
    backend.temperature = 0.0;
    backend.timeoutSeconds = config_.timeoutSeconds.value();
    return backend;
}

bool ChineseLlm::isLlmInputMethod(fcitx::InputContext *ic) const {
    const auto *entry = instance_->inputMethodEntry(ic);
    return entry && entry->uniqueName() == "llm-pinyin" &&
           entry->addon() == "chinese-llm";
}

bool ChineseLlm::isEligible(fcitx::InputContext *ic) {
    if (!config_.enabled.value() || !ic->hasFocus()) {
        return false;
    }
    // Never look at a password field, even to read the preedit.
    if (ic->capabilityFlags().testAny(
            fcitx::CapabilityFlag::PasswordOrSensitive)) {
        return false;
    }
    // Neither normal pinyin nor shuangpin is opted into model requests.
    if (!isLlmInputMethod(ic)) {
        return false;
    }
    auto candidateList = ic->inputPanel().candidateList();
    // Stroke/action filters deliberately make CommonCandidateList immutable.
    // Do not send or insert model output while the engine owns such a view.
    if (!candidateList || candidateList->empty() ||
        !candidateList->toModifiable()) {
        return false;
    }
    if (inCooldown()) {
        return false;
    }
    expireCooldown();
    return true;
}

std::string ChineseLlm::buildContext(fcitx::InputContext *ic) const {
    const int limit = config_.contextLength.value();
    if (limit <= 0) {
        return {};
    }
    const auto &surrounding = ic->surroundingText();
    if (!surrounding.isValid()) {
        // Many clients cannot provide surrounding text. Degrading to an empty
        // context is correct: the model still works, just less well.
        return {};
    }
    const std::string &text = surrounding.text();
    const unsigned int cursor = surrounding.cursor();
    const size_t length = fcitx::utf8::lengthValidated(text);
    if (length == fcitx::utf8::INVALID_LENGTH || cursor == 0 ||
        cursor > length) {
        return {};
    }
    const size_t usable = std::min<size_t>(cursor, static_cast<size_t>(limit));
    const auto start = utf8::nextNChar(text.begin(), cursor - usable);
    const auto end = utf8::nextNChar(text.begin(), cursor);
    return std::string(start, end);
}

std::string ChineseLlm::currentPinyin(fcitx::InputContext *ic) const {
    // CommitPreview uses Han client preedit alongside raw server preedit.
    // Prefer the server field when present; both are mixed after partial
    // selection.
    auto preedit = ic->inputPanel().preedit().toString();
    if (preedit.empty()) {
        preedit = ic->inputPanel().clientPreedit().toString();
    }
    if (!isPlainPinyin(preedit)) {
        return {};
    }
    std::replace(preedit.begin(), preedit.end(), ' ', '\'');
    if (preedit.size() > MAX_PINYIN_LENGTH ||
        static_cast<int>(preedit.size()) < config_.minPinyinLength.value()) {
        return {};
    }
    return preedit;
}

std::string ChineseLlm::cacheKey(const std::string &pinyin,
                                 const std::string &context) {
    // Plain pinyin never contains NUL, so this delimiter is unambiguous even
    // when preceding text itself contains arbitrary bytes.
    return pinyin + '\0' + context;
}

void ChineseLlm::removeCandidates(fcitx::InputContext *ic, LlmState *state) {
    auto list = ic->inputPanel().candidateList();
    auto *modifiable = list ? list->toModifiable() : nullptr;
    if (!modifiable) {
        return;
    }
    bool removed = false;
    for (int i = modifiable->totalSize() - 1; i >= 0; --i) {
        if (dynamic_cast<const LlmCandidateWord *>(
                &modifiable->candidateFromAll(i))) {
            modifiable->remove(i);
            removed = true;
        }
    }
    if (removed) {
        state->inserting = true;
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        state->inserting = false;
    }
}

void ChineseLlm::invalidateState(fcitx::InputContext *ic, LlmState *state) {
    state->generation++;
    state->debounce.reset();
    if (state->requestSeq && state->requestSeq == worker_->currentSeq()) {
        worker_->cancelAll();
    }
    state->requestSeq = 0;
    state->pinyin.clear();
    state->context.clear();
    state->candidates.clear();
    removeCandidates(ic, state);
}

void ChineseLlm::invalidateAll() {
    worker_->cancelAll();
    instance_->inputContextManager().foreach([this](InputContext *ic) {
        invalidateState(ic, ic->propertyFor(&factory_));
        return true;
    });
}

void ChineseLlm::handleUpdateUI(fcitx::Event &event) {
    auto &uiEvent = static_cast<fcitx::InputContextUpdateUIEvent &>(event);
    if (uiEvent.component() != fcitx::UserInterfaceComponent::InputPanel) {
        return;
    }
    fcitx::InputContext *ic = uiEvent.inputContext();
    if (!ic) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    // Our own insertCandidates() ends with an update request; this is how that
    // second visit is stopped before it becomes a loop.
    if (state->inserting) {
        return;
    }
    if (!isEligible(ic)) {
        invalidateState(ic, state);
        return;
    }
    auto pinyin = currentPinyin(ic);
    if (pinyin.empty()) {
        invalidateState(ic, state);
        return;
    }
    auto context = buildContext(ic);
    if (state->pinyin == pinyin && state->context == context) {
        // The engine can rebuild the list without changing the composition,
        // for example when paging or moving its cursor.
        insertCandidates(ic, state, state->candidates);
        return;
    }
    invalidateState(ic, state);
    state->pinyin = pinyin;
    state->context = context;

    const auto key = cacheKey(pinyin, context);
    if (const auto iter = cache_.find(key); iter != cache_.end()) {
        std::erase(cacheOrder_, key);
        cacheOrder_.push_back(key);
        state->candidates = iter->second;
        insertCandidates(ic, state, state->candidates);
        return;
    }

    scheduleRequest(ic, state, std::move(pinyin), std::move(context));
}

void ChineseLlm::scheduleRequest(fcitx::InputContext *ic, LlmState *state,
                                 std::string pinyin, std::string context) {
    const int debounce = config_.debounceMs.value();
    if (debounce <= 0) {
        dispatchRequest(ic, state, std::move(pinyin), std::move(context),
                        state->generation);
        return;
    }

    // Debounce before submission, so a burst of typing produces one request
    // instead of starting work the server may continue even after curl aborts.
    state->debounce = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        fcitx::now(CLOCK_MONOTONIC) + static_cast<uint64_t>(debounce) * 1000,
        static_cast<uint64_t>(debounce) * 1000,
        [this, owner = watch(), ref = ic->watch(),
         generation = state->generation, pinyin = std::move(pinyin),
         context = std::move(context)](fcitx::EventSourceTime *,
                                       uint64_t) mutable {
            auto *inputContext = ref.get();
            if (!owner.isValid() || !inputContext) {
                return true;
            }
            auto *cur = inputContext->propertyFor(&factory_);
            dispatchRequest(inputContext, cur, std::move(pinyin),
                            std::move(context), generation);
            return true;
        });
    state->debounce->setOneShot();
}

void ChineseLlm::dispatchRequest(fcitx::InputContext *ic, LlmState *state,
                                 std::string pinyin, std::string context,
                                 uint64_t generation) {
    if (state->generation != generation || !isEligible(ic) ||
        currentPinyin(ic) != pinyin || buildContext(ic) != context) {
        return;
    }
    // Create references on the main thread and only dereference them there.
    // The dispatcher outlives the addon; queued callbacks also track the addon,
    // not only the client, so unloading cannot leave a raw-this callback
    // behind.
    auto ref = ic->watch();
    state->requestSeq = worker_->submit(
        backendConfig(), std::move(pinyin), context,
        [dispatcher = &dispatcher_, owner = watch(), ref, context,
         generation](FetchResult result) {
            dispatcher->schedule([owner, ref, context, generation,
                                  result = std::move(result)]() {
                auto *self = owner.get();
                auto *ic = ref.get();
                if (!self || !ic) {
                    return;
                }
                auto *state = ic->propertyFor(&self->factory_);
                if (result.seq != self->worker_->currentSeq() ||
                    state->generation != generation || !self->isEligible(ic) ||
                    self->currentPinyin(ic) != result.pinyin ||
                    self->buildContext(ic) != context) {
                    return;
                }
                state->requestSeq = 0;
                if (!result.ok) {
                    self->noteError(result.error);
                    return;
                }
                self->errorCount_ = 0;
                state->candidates = result.candidates;
                if (result.candidates.empty()) {
                    return;
                }
                self->rememberCandidates(cacheKey(result.pinyin, context),
                                         result.candidates);
                self->insertCandidates(ic, state, state->candidates);
            });
        });
}

void ChineseLlm::rememberCandidates(
    const std::string &key, const std::vector<std::string> &candidates) {
    const int cacheSize = config_.cacheSize.value();
    if (cacheSize <= 0) {
        return;
    }
    cache_.insert_or_assign(key, candidates);
    std::erase(cacheOrder_, key);
    cacheOrder_.push_back(key);
    while (static_cast<int>(cacheOrder_.size()) > cacheSize) {
        cache_.erase(cacheOrder_.front());
        cacheOrder_.erase(cacheOrder_.begin());
    }
}

void ChineseLlm::insertCandidates(fcitx::InputContext *ic, LlmState *state,
                                  const std::vector<std::string> &candidates) {
    if (candidates.empty()) {
        return;
    }
    auto candidateList = ic->inputPanel().candidateList();
    if (!candidateList) {
        return;
    }
    auto *modifiable = candidateList->toModifiable();
    if (!modifiable) {
        return;
    }

    const int total = modifiable->totalSize();
    if (total <= 0) {
        return;
    }
    const int wanted =
        std::min<int>(candidates.size(), config_.maxCandidates.value());
    const bool badge = config_.showBadge.value();

    // Collect what is already on screen so a model reading that duplicates a
    // dictionary reading does not appear twice.
    std::vector<std::string> existing;
    existing.reserve(total);
    for (int i = 0; i < total; i++) {
        existing.push_back(modifiable->candidateFromAll(i).text().toString());
    }

    std::vector<std::string> toInsert;
    toInsert.reserve(wanted);
    for (const auto &candidate : candidates) {
        if (static_cast<int>(toInsert.size()) >= wanted) {
            break;
        }
        if (std::ranges::find(existing, candidate) != existing.end()) {
            continue;
        }
        if (std::ranges::find(toInsert, candidate) != toInsert.end()) {
            continue;
        }
        toInsert.push_back(candidate);
    }
    if (toInsert.empty()) {
        return;
    }

    // The config position is 1-based, matching cloudpinyin's CandidateIndex.
    const int position =
        std::clamp(config_.candidateIndex.value() - 1, 0, total);

    const auto valid = [owner = watch(), ref = ic->watch(),
                        generation = state->generation, pinyin = state->pinyin,
                        context = state->context](InputContext *selectedIc) {
        auto *self = owner.get();
        auto *ic = ref.get();
        if (!self || !ic || selectedIc != ic || !self->isEligible(ic)) {
            return false;
        }
        return ic->propertyFor(&self->factory_)->generation == generation &&
               self->currentPinyin(ic) == pinyin &&
               self->buildContext(ic) == context;
    };
    state->inserting = true;
    // Insert back to front so the first candidate ends up first on screen.
    for (int i = static_cast<int>(toInsert.size()) - 1; i >= 0; i--) {
        modifiable->insert(position, std::make_unique<LlmCandidateWord>(
                                         toInsert[i], badge, valid));
    }
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    state->inserting = false;
}

void ChineseLlm::handleKeyEvent(fcitx::Event &event) {
    auto &keyEvent = static_cast<fcitx::KeyEvent &>(event);
    if (keyEvent.isRelease() || !isLlmInputMethod(keyEvent.inputContext())) {
        return;
    }
    if (!keyEvent.key().checkKeyList(config_.toggleKey.value())) {
        return;
    }
    keyEvent.filterAndAccept();
    toggle();
}

void ChineseLlm::toggle() {
    config_.enabled.setValue(!config_.enabled.value());
    fcitx::safeSaveAsIni(config_, "conf/chinese-llm.conf");
    invalidateAll();
    errorCount_ = 0;
    cooldownUntil_ = {};
    refreshStatus();
    FCITX_INFO() << "chinese-llm: "
                 << (config_.enabled.value() ? "enabled" : "disabled");
}

void ChineseLlm::refreshStatus() {
    const bool enabled = config_.enabled.value();
    // Kimpanel does not serialize checked state, so spell out the switch state
    // in the visible short text as well. Avoid ASCII ':' (protocol delimiter).
    toggleAction_.setShortText(
        enabled ? D_("chinese-llm", "LLM candidates enabled")
                : D_("chinese-llm", "LLM candidates disabled"));
    toggleAction_.setLongText(
        D_("chinese-llm",
           "Click or use the toggle shortcut to enable or disable LLM "
           "candidates. Native pinyin remains available when disabled."));
    toggleAction_.setIcon(enabled ? "weather-overcast" : "media-playback-stop");
    toggleAction_.setChecked(enabled);
    toggleAction_.setHotkey(config_.toggleKey.value());
    instance_->inputContextManager().foreach([this](InputContext *ic) {
        if (isLlmInputMethod(ic) && ic->statusArea().isChild(&toggleAction_)) {
            // StatusArea refresh also republishes the AI/PY input method label.
            // Do not rebuild GNOME's status menu on every keystroke.
            toggleAction_.update(ic);
        }
        return true;
    });
}

void ChineseLlm::noteError(const std::string &error) {
    errorCount_++;
    FCITX_LOGC(chinesellm, Debug)
        << "chinese-llm: request failed (" << errorCount_ << "): " << error;
    if (errorCount_ >= config_.maxConsecutiveErrors.value()) {
        // Stop hammering a dead endpoint. Typing keeps working because the
        // worker is bypassed entirely while cooling down.
        const int seconds = config_.cooldownSeconds.value();
        cooldownUntil_ =
            std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        FCITX_WARN() << "chinese-llm: pausing requests for " << seconds
                     << " seconds after " << errorCount_ << " failures.";
    }
}

bool ChineseLlm::inCooldown() const {
    if (cooldownUntil_ == std::chrono::steady_clock::time_point{}) {
        return false;
    }
    return std::chrono::steady_clock::now() < cooldownUntil_;
}

void ChineseLlm::expireCooldown() {
    if (cooldownUntil_ != std::chrono::steady_clock::time_point{} &&
        !inCooldown()) {
        cooldownUntil_ = {};
        errorCount_ = 0;
    }
}

} // namespace fcitx::chinesellm

// The loader derives the V2 symbol name from the addon name, which here is
// "chinese-llm" -- a hyphen is not a valid C identifier, so that symbol can
// never exist. It then falls back to the unsuffixed
// "fcitx_addon_factory_instance", which is why the backwards-compatible macro
// is required rather than optional.
FCITX_ADDON_FACTORY_V2_BACKWARDS(chinese_llm,
                                 fcitx::chinesellm::ChineseLlmFactory);
