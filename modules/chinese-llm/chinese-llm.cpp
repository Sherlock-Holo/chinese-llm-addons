/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "chinese-llm.h"
#include "llmbackend.h"
#include "llmworker.h"
#include "native/pinyin/pinyin.h"
#include "notifications_public.h"
#include <algorithm>
#include <array>
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
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
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
constexpr char kDefaultBadge[] = "\u2601\uFE0E";
/// Default for a dictionary candidate that the model also produced: the gear
/// stands for "the machine agrees". A check mark was dropped because people type
/// it as real text, and an intersection sign because it was too abstract. U+2699
/// has an emoji presentation, so the text variation selector asks for the
/// monochrome glyph, like the cloud. Both symbols are user-configurable.
constexpr char kDefaultOverlapBadge[] = "\u2699\uFE0E";

/// CandidateWord keeps setComment() protected because only a candidate should
/// decide its own comment. The dictionary candidates belong to the private
/// native engine, so the only way to annotate them in place (without
/// re-implementing their selection logic) is to reach the member through a
/// derived class, which is exactly what protected access allows.
struct CandidateComment : fcitx::CandidateWord {
    static void set(const fcitx::CandidateWord &word, fcitx::Text comment) {
        auto &mutableWord = const_cast<fcitx::CandidateWord &>(word);
        (mutableWord.*(&CandidateComment::setComment))(std::move(comment));
    }
};

bool endsWith(const std::string &text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

/// Add the overlap mark after any comment the dictionary candidate already has.
/// Returns whether anything changed, so repeated panel refreshes are no-ops.
bool markOverlap(const fcitx::CandidateWord &word, const std::string &symbol) {
    auto comment = word.comment().toString();
    if (endsWith(comment, symbol)) {
        return false;
    }
    if (!comment.empty()) {
        comment += ' ';
    }
    comment += symbol;
    CandidateComment::set(word, fcitx::Text(comment));
    return true;
}

bool unmarkOverlap(const fcitx::CandidateWord &word,
                   const std::string &symbol) {
    auto comment = word.comment().toString();
    if (symbol.empty() || !endsWith(comment, symbol)) {
        return false;
    }
    comment.resize(comment.size() - symbol.size());
    if (endsWith(comment, " ")) {
        comment.pop_back();
    }
    CandidateComment::set(word, fcitx::Text(comment));
    return true;
}
/// Quiet period before an opened history snapshot is rewritten.
constexpr uint64_t kSnapshotDelayUs = 300 * 1000;

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
    LlmCandidateWord(std::string text, std::string prefix,
                     const std::string &badge,
                     std::function<bool(fcitx::InputContext *)> valid,
                     std::function<void()> adopted)
        : text_(std::move(text)), prefix_(std::move(prefix)),
          valid_(std::move(valid)), adopted_(std::move(adopted)) {
        setText(fcitx::Text(text_));
        if (!badge.empty()) {
            setComment(fcitx::Text(badge));
            setSpaceBetweenComment(true);
        }
    }

    void select(fcitx::InputContext *inputContext) const override {
        if (!valid_(inputContext)) {
            return;
        }
        // Hanzi accepted earlier in this composition were never committed; only
        // this reset would throw them away, so they go out in front.
        inputContext->commitString(prefix_ + text_);
        if (adopted_) {
            adopted_();
        }
        inputContext->inputPanel().reset();
        inputContext->reset();
    }

private:
    std::string text_;
    std::string prefix_;
    std::function<bool(fcitx::InputContext *)> valid_;
    std::function<void()> adopted_;
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
    requestsAction_.registerAction("chinese-llm-stat-requests",
                                   &instance_->userInterfaceManager());
    overlapAction_.registerAction("chinese-llm-stat-overlap",
                                  &instance_->userInterfaceManager());
    rateAction_.registerAction("chinese-llm-stat-rate",
                               &instance_->userInterfaceManager());
    requestsAction_.setIcon("view-statistics");
    overlapAction_.setIcon("view-statistics");
    rateAction_.setIcon("view-statistics");
    requestsAction_.setLongText(
        D_("chinese-llm",
           "Requests started in this Fcitx session, including failures and "
           "cancellations. Valid requests returned usable candidates for the "
           "current input. Cache hits are not requests."));
    overlapAction_.setLongText(
        D_("chinese-llm",
           "Valid requests with at least one candidate identical to any native "
           "candidate. Each request is counted once, before applying the "
           "candidate "
           "display limit."));
    rateAction_.setLongText(D_(
        "chinese-llm",
        "Overlapping requests divided by valid requests. No rate is shown "
        "until "
        "a valid request completes. Statistics reset when Fcitx restarts; no "
        "input text is saved."));
    historyAction_.registerAction("chinese-llm-history",
                                  &instance_->userInterfaceManager());
    clearHistoryAction_.registerAction("chinese-llm-history-clear",
                                       &instance_->userInterfaceManager());
    historyAction_.setIcon("document-open-recent");
    historyAction_.setLongText(
        D_("chinese-llm",
           "View inputs, model replies, latency, adoption and native "
           "overlap. Opens a private local snapshot in your browser."));
    clearHistoryAction_.setIcon("edit-clear-history");
    clearHistoryAction_.setShortText(
        D_("chinese-llm", "Clear LLM request history"));
    historyConnection_ = historyAction_.connect<SimpleAction::Activated>(
        [this](InputContext *ic) {
            if (ic && isLlmInputMethod(ic) &&
                ic->statusArea().isChild(&historyAction_)) {
                openHistory();
            }
        });
    clearHistoryConnection_ =
        clearHistoryAction_.connect<SimpleAction::Activated>(
            [this](InputContext *ic) {
                if (ic && isLlmInputMethod(ic) &&
                    ic->statusArea().isChild(&clearHistoryAction_)) {
                    clearHistory();
                }
            });
    refreshStatistics();
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
          EventType::InputContextCapabilityAboutToChange}) {
        eventWatchers_.emplace_back(instance_->watchEvent(
            type, EventWatcherPhase::PreInputMethod, [this](Event &event) {
                auto *ic =
                    static_cast<InputContextEvent &>(event).inputContext();
                if (event.type() ==
                        EventType::InputContextCapabilityAboutToChange &&
                    static_cast<CapabilityAboutToChangeEvent &>(event)
                        .newFlags()
                        .testAny(CapabilityFlag::PasswordOrSensitive)) {
                    clearHistory();
                }
                const auto reason =
                    event.type() == EventType::InputContextFocusOut
                        ? "focus_lost"
                    : event.type() == EventType::InputContextReset
                        ? "composition_reset"
                    : event.type() == EventType::InputContextSwitchInputMethod
                        ? "input_method_changed"
                        : "capabilities_changed";
                invalidateState(ic, ic->propertyFor(&factory_),
                                event.type() == EventType::InputContextReset,
                                reason);
            }));
    }
    eventWatchers_.emplace_back(instance_->watchEvent(
        EventType::InputContextSurroundingTextUpdated,
        EventWatcherPhase::PreInputMethod, [this](Event &event) {
            auto *ic = static_cast<InputContextEvent &>(event).inputContext();
            auto *state = ic->propertyFor(&factory_);
            // Surrounding-text notifications are not composition boundaries.
            // Frontends routinely repeat the same value after preedit updates.
            // Invalidate only when the effective context sent to the model
            // differs; otherwise preserve pending timers and visible results.
            if (!isLlmInputMethod(ic) ||
                ic->capabilityFlags().testAny(
                    CapabilityFlag::PasswordOrSensitive) ||
                state->pinyin.empty() ||
                buildContext(ic, state->selected) == state->context) {
                return;
            }
            invalidateState(ic, state, false, "surrounding_context_changed");
            // A surrounding update need not be followed by an InputPanel UI
            // event. Retry the still-current composition on the main thread,
            // using fresh context and all the usual eligibility/privacy guards.
            dispatcher_.schedule([owner = watch(), ref = ic->watch()] {
                if (auto *self = owner.get()) {
                    if (auto *ic = ref.get(); ic && self->isEligible(ic)) {
                        ic->updateUserInterface(
                            UserInterfaceComponent::InputPanel);
                    }
                }
            });
        }));
    eventWatchers_.emplace_back(instance_->watchEvent(
        EventType::InputContextKeyEvent, EventWatcherPhase::PreInputMethod,
        [this](Event &event) {
            auto *state =
                static_cast<KeyEvent &>(event).inputContext()->propertyFor(
                    &factory_);
            if (state->pinyin.empty()) {
                state->commitHistoryId = 0;
                state->commitSelected.clear();
            }
        }));
    eventWatchers_.emplace_back(instance_->watchEvent(
        EventType::InputContextCommitString, EventWatcherPhase::PostInputMethod,
        [this](Event &event) {
            auto &commit = static_cast<CommitStringEvent &>(event);
            auto *ic = commit.inputContext();
            if (!config_.recordHistory.value() || !isLlmInputMethod(ic) ||
                ic->capabilityFlags().testAny(
                    CapabilityFlag::PasswordOrSensitive)) {
                return;
            }
            auto *state = ic->propertyFor(&factory_);
            const auto id =
                state->historyId ? state->historyId : state->commitHistoryId;
            // The engine commits the whole sentence, accepted hanzi included;
            // the request was only about what came after them.
            const auto &selected =
                state->historyId ? state->selected : state->commitSelected;
            std::string_view text = commit.text();
            if (text.starts_with(selected)) {
                text.remove_prefix(selected.size());
            }
            if (history_.adoptable(id)) {
                history_.adopt(id, std::string(text), true);
                refreshHistory();
            }
            state->commitHistoryId = 0;
            state->commitSelected.clear();
        }));

    worker_->start();
    FCITX_INFO() << "chinese-llm: loaded, endpoint "
                 << config_.endpoint.value();
}

ChineseLlm::~ChineseLlm() {
    toggleConnection_.disconnect();
    historyConnection_.disconnect();
    clearHistoryConnection_.disconnect();
    eventWatchers_.clear();
    invalidateAll();
    worker_->stop();
    clearHistory();
}

void ChineseLlm::keyEvent(const InputMethodEntry &, KeyEvent &event) {
    pinyin_->keyEvent(pinyinEntry_, event);
}

void ChineseLlm::activate(const InputMethodEntry &, InputContextEvent &event) {
    auto *ic = event.inputContext();
    invalidateState(ic, ic->propertyFor(&factory_));
    pinyin_->activate(pinyinEntry_, event);
    // Refresh before attaching: a transfer may have started while another IM
    // was active. Every row belongs only to our own InputMethod status group.
    refreshStatistics();
    refreshHistory();
    for (auto *action : {&toggleAction_, &requestsAction_, &overlapAction_,
                         &rateAction_, &historyAction_, &clearHistoryAction_}) {
        ic->statusArea().addAction(StatusGroup::InputMethod, action);
    }
    toggleAction_.update(ic);
}

void ChineseLlm::deactivate(const InputMethodEntry &,
                            InputContextEvent &event) {
    auto *ic = event.inputContext();
    // Core can invoke deactivation before our boundary watcher. Retain an
    // accurate category rather than losing it to a generic cancellation.
    invalidateState(ic, ic->propertyFor(&factory_), false,
                    "input_method_deactivated");
    pinyin_->deactivate(pinyinEntry_, event);
    // Retain the selected IM's action across transient focus loss (shell
    // menus). Fcitx clears the InputMethod status group before activating
    // another IM.
}

void ChineseLlm::reset(const InputMethodEntry &, InputContextEvent &event) {
    auto *ic = event.inputContext();
    invalidateState(ic, ic->propertyFor(&factory_), true, "composition_reset");
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
    if (!config_.recordHistory.value()) {
        clearHistory();
    }
    refreshHistory();
    refreshStatus();
}

void ChineseLlm::notifyUser(const std::string &tipId,
                            const std::string &summary,
                            const std::string &body) {
    if (auto *notifications = this->notifications()) {
        notifications->call<fcitx::INotifications::showTip>(
            tipId, _("LLM Pinyin"), "", summary, body, 5000);
    }
}

void ChineseLlm::rejectInvalidSymbols(fcitx::RawConfig &config) {
    struct Field {
        const char *path;
        const char *label;
        const char *defaultSymbol;
    };
    for (const auto &[path, label, defaultSymbol] :
         {Field{"BadgeSymbol", D_("chinese-llm", "Symbol for LLM candidates"),
                kDefaultBadge},
          Field{"OverlapSymbol",
                D_("chinese-llm", "Symbol for agreeing candidates"),
                kDefaultOverlapBadge}}) {
        const auto *value = config.valueByPath(path);
        if (!value) {
            continue;
        }
        const char *reason = nullptr;
        switch (checkBadgeSymbol(*value)) {
        case BadgeSymbolError::None:
            continue;
        case BadgeSymbolError::Empty:
            // Clearing the field means "restore the default". It is stored as
            // the default, so the settings page shows it filled in again.
            config.setValueByPath(path, defaultSymbol);
            continue;
        case BadgeSymbolError::TooLong:
            reason = _("The symbol may have at most four characters.");
            break;
        case BadgeSymbolError::Unsafe:
            reason = _("The symbol must not contain a colon, a control "
                       "character, invalid text or blanks at either end.");
            break;
        }
        // Missing from the incoming configuration means "unchanged" for a
        // partial load, so the previous symbol is kept rather than reset.
        config.remove(path);
        FCITX_WARN() << "chinese-llm: rejected invalid " << path;
        const std::string field = _(label);
        notifyUser("chinese-llm-invalid-symbol",
                   std::vformat(_("{0} was not saved"),
                                std::make_format_args(field)),
                   reason);
    }
}

void ChineseLlm::setConfig(const fcitx::RawConfig &incoming) {
    RawConfig config = incoming;
    rejectInvalidSymbols(config);
    RawConfig before;
    config_.save(before);
    config_.load(config, true);
    RawConfig after;
    config_.save(after);
    // Neither switch changes what is requested, so they must not reset the
    // composition. The overlap mark follows the next panel refresh.
    for (const char *name :
         {"RecordHistory", "ShowOverlapBadge", "OverlapSymbol"}) {
        before.remove(name);
        after.remove(name);
    }
    fcitx::safeSaveAsIni(config_, "conf/chinese-llm.conf");
    if (!config_.recordHistory.value()) {
        clearHistory();
    }
    refreshHistory();
    if (before == after) {
        return;
    }
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
    backend.englishPolicy = config_.englishPolicy.value();
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

std::string ChineseLlm::buildContext(fcitx::InputContext *ic,
                                     const std::string &selected) const {
    const int limit = config_.contextLength.value();
    if (limit <= 0) {
        return {};
    }
    std::string text;
    const auto &surrounding = ic->surroundingText();
    // Many clients cannot provide surrounding text. Degrading to what the
    // user accepted in this composition (possibly nothing) is correct: the
    // model still works, just less well.
    if (surrounding.isValid()) {
        const std::string &all = surrounding.text();
        const unsigned int cursor = surrounding.cursor();
        const size_t length = fcitx::utf8::lengthValidated(all);
        if (length != fcitx::utf8::INVALID_LENGTH && cursor != 0 &&
            cursor <= length) {
            const size_t usable =
                std::min<size_t>(cursor, static_cast<size_t>(limit));
            const auto start = utf8::nextNChar(all.begin(), cursor - usable);
            const auto end = utf8::nextNChar(all.begin(), cursor);
            text.assign(start, end);
        }
    }
    // Accepted hanzi are not in the client's text yet but come right before
    // what is being typed.
    text += selected;
    const size_t length = fcitx::utf8::lengthValidated(text);
    if (length == fcitx::utf8::INVALID_LENGTH) {
        return {};
    }
    if (length > static_cast<size_t>(limit)) {
        text.erase(0, std::distance(text.begin(),
                                    utf8::nextNChar(text.begin(),
                                                    length - limit)));
    }
    return text;
}

Composition ChineseLlm::currentComposition(fcitx::InputContext *ic) const {
    // The private engine is the source of truth. The panel's preedit mixes the
    // accepted hanzi and the remaining pinyin after a partial selection, so
    // the composition is read directly and split at the accepted hanzi.
    auto *engine = static_cast<llmnative::PinyinEngine *>(pinyin_.get());
    auto *engineState = ic->propertyFor(&engine->factory());
    if (engineState->mode_ != llmnative::PinyinMode::Normal) {
        return {};
    }
    const auto &composition = engineState->context_;
    if (composition.selectedLength() >= composition.size()) {
        return {};
    }
    // Same text the panel shows (syllables separated), which is what the
    // length policy and the prompt are written for.
    auto [preedit, cursor] = composition.preeditWithCursor();
    std::string selected = composition.selectedSentence();
    if (!preedit.starts_with(selected)) {
        return {};
    }
    // Native candidates are built for the pinyin before the cursor (all of it
    // when the cursor is not inside the unselected part), so the comparison has
    // to be too.
    std::string pinyin = preedit.substr(
        selected.size(),
        cursor > selected.size() ? cursor - selected.size() : std::string::npos);
    // The engine may put a separator between the hanzi and the pinyin.
    const auto first = pinyin.find_first_not_of(" '");
    const auto last = pinyin.find_last_not_of(" '");
    if (first == std::string::npos) {
        return {};
    }
    pinyin = pinyin.substr(first, last - first + 1);
    if (!isPlainPinyin(pinyin)) {
        return {};
    }
    std::replace(pinyin.begin(), pinyin.end(), ' ', '\'');
    if (pinyin.size() > MAX_PINYIN_LENGTH ||
        static_cast<int>(pinyin.size()) < config_.minPinyinLength.value()) {
        return {};
    }
    return {std::move(pinyin), std::move(selected)};
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
        const auto &word = modifiable->candidateFromAll(i);
        if (dynamic_cast<const LlmCandidateWord *>(&word)) {
            modifiable->remove(i);
            removed = true;
        } else if (unmarkOverlap(word, appliedOverlapSymbol_)) {
            // The model no longer vouches for this candidate.
            removed = true;
        }
    }
    if (removed) {
        state->inserting = true;
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        state->inserting = false;
    }
}

void ChineseLlm::invalidateState(fcitx::InputContext *ic, LlmState *state,
                                 bool preserveCommit, std::string_view reason) {
    if (preserveCommit && state->historyId) {
        state->commitHistoryId = state->historyId;
        state->commitSelected = state->selected;
    } else if (!preserveCommit) {
        state->commitHistoryId = 0;
        state->commitSelected.clear();
    }
    if (state->historyId) {
        history_.markCancelled(state->historyId, std::string(reason));
        refreshHistory();
    }
    state->historyId = 0;
    state->generation++;
    state->debounce.reset();
    if (state->requestSeq && state->requestSeq == worker_->currentSeq()) {
        worker_->cancelAll();
    }
    state->requestSeq = 0;
    state->pinyin.clear();
    state->context.clear();
    state->selected.clear();
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
    // Real frontends can block commit events while native selection already
    // clears the panel. Keep its history identity until the queued commit is
    // delivered, or until the next key starts an unrelated composition.
    const bool emptyOwnComposition =
        isLlmInputMethod(ic) && config_.enabled.value() &&
        !ic->capabilityFlags().testAny(CapabilityFlag::PasswordOrSensitive) &&
        ic->inputPanel().preedit().empty() &&
        ic->inputPanel().clientPreedit().empty();
    if (!isEligible(ic)) {
        invalidateState(ic, state, emptyOwnComposition, "input_not_eligible");
        return;
    }
    auto [pinyin, selected] = currentComposition(ic);
    if (pinyin.empty()) {
        invalidateState(ic, state, emptyOwnComposition,
                        "input_not_plain_pinyin");
        return;
    }
    auto context = buildContext(ic, selected);
    if (state->pinyin == pinyin && state->context == context &&
        state->selected == selected) {
        // The engine can rebuild the list without changing the composition,
        // for example when paging or moving its cursor.
        insertCandidates(ic, state, state->candidates);
        return;
    }
    invalidateState(ic, state, false, "input_keys_or_context_changed");
    state->pinyin = pinyin;
    state->context = context;
    state->selected = selected;

    const auto key = cacheKey(pinyin, context);
    if (const auto iter = cache_.find(key); iter != cache_.end()) {
        std::erase(cacheOrder_, key);
        cacheOrder_.push_back(key);
        state->candidates = iter->second;
        if (config_.recordHistory.value()) {
            state->historyId = history_.recordCacheHit(
                pinyin, config_.model.value(), state->candidates,
                overlapsNative(ic, state->candidates));
            refreshHistory();
        }
        insertCandidates(ic, state, state->candidates);
        return;
    }

    scheduleRequest(ic, state, std::move(pinyin), std::move(context),
                    std::move(selected));
}

void ChineseLlm::scheduleRequest(fcitx::InputContext *ic, LlmState *state,
                                 std::string pinyin, std::string context,
                                 std::string selected) {
    const int debounce = config_.debounceMs.value();
    if (debounce <= 0) {
        dispatchRequest(ic, state, std::move(pinyin), std::move(context),
                        std::move(selected), state->generation);
        return;
    }

    // Debounce before submission, so a burst of typing produces one request
    // instead of starting work the server may continue even after curl aborts.
    state->debounce = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        fcitx::now(CLOCK_MONOTONIC) + static_cast<uint64_t>(debounce) * 1000,
        // Accuracy is how late the timer may fire, not its interval. The
        // event loop's default (and the debounce value itself) lets it slip
        // by up to that much again, doubling the configured delay.
        1000,
        [this, owner = watch(), ref = ic->watch(),
         generation = state->generation, pinyin = std::move(pinyin),
         context = std::move(context),
         selected = std::move(selected)](fcitx::EventSourceTime *,
                                         uint64_t) mutable {
            auto *inputContext = ref.get();
            if (!owner.isValid() || !inputContext) {
                return true;
            }
            auto *cur = inputContext->propertyFor(&factory_);
            dispatchRequest(inputContext, cur, std::move(pinyin),
                            std::move(context), std::move(selected),
                            generation);
            return true;
        });
    state->debounce->setOneShot();
}

bool ChineseLlm::matchesInput(fcitx::InputContext *ic,
                              const InputSnapshot &snapshot) {
    if (!isEligible(ic) ||
        ic->propertyFor(&factory_)->generation != snapshot.generation) {
        return false;
    }
    const auto composition = currentComposition(ic);
    return composition.pinyin == snapshot.pinyin &&
           composition.selected == snapshot.selected &&
           buildContext(ic, composition.selected) == snapshot.context;
}

void ChineseLlm::dispatchRequest(fcitx::InputContext *ic, LlmState *state,
                                 std::string pinyin, std::string context,
                                 std::string selected, uint64_t generation) {
    InputSnapshot snapshot{generation, std::move(pinyin), std::move(context),
                           std::move(selected)};
    if (state->generation != generation || !matchesInput(ic, snapshot)) {
        return;
    }
    // Create references on the main thread and only dereference them there.
    // The dispatcher outlives the addon; queued callbacks also track the addon,
    // not only the client, so unloading cannot leave a raw-this callback
    // behind.
    const auto historyId =
        config_.recordHistory.value()
            ? history_.begin(snapshot.pinyin, config_.model.value())
            : 0;
    state->historyId = historyId;
    state->requestSeq = worker_->submit(
        backendConfig(), snapshot.pinyin, snapshot.context,
        [dispatcher = &dispatcher_, owner = watch(), ref = ic->watch(),
         snapshot, historyId](FetchStage stage, const FetchResult &result) {
            // Any thread. Every event, including cancellations, reaches the
            // main thread, where one place decides what it still means.
            dispatcher->schedule(
                [owner, ref, snapshot, historyId, stage, result]() {
                    if (auto *self = owner.get()) {
                        self->handleFetch(ref.get(), snapshot, historyId, stage,
                                          result);
                    }
                });
        });
    history_.setSeq(historyId, state->requestSeq);
    refreshHistory();
}

void ChineseLlm::handleFetch(fcitx::InputContext *ic,
                             const InputSnapshot &snapshot, uint64_t historyId,
                             FetchStage stage, const FetchResult &result) {
    if (stage == FetchStage::Started) {
        // Start notifications survive invalidation, but never recreate a
        // history row that the user cleared or privacy handling removed.
        history_.markStarted(historyId);
        refreshHistory();
        refreshStatistics();
        return;
    }

    const bool current = ic && !result.cancelled &&
                         result.seq == worker_->currentSeq() &&
                         matchesInput(ic, snapshot);
    std::optional<bool> overlap;
    if (current && result.ok && !result.candidates.empty()) {
        overlap = overlapsNative(ic, result.candidates);
    }

    if (config_.recordHistory.value() && history_.find(historyId)) {
        if (ic && ic->capabilityFlags().testAny(
                      CapabilityFlag::PasswordOrSensitive)) {
            clearHistory();
        } else {
            history_.finish(historyId, result);
            if (overlap) {
                history_.markUsable(historyId, *overlap);
            } else if (result.ok && !result.cancelled && !current) {
                history_.markStale(historyId);
            }
            refreshHistory();
        }
    }
    if (!current) {
        return;
    }

    auto *state = ic->propertyFor(&factory_);
    state->requestSeq = 0;
    if (!result.ok) {
        noteError(result.error);
        return;
    }
    errorCount_ = 0;
    state->candidates = result.candidates;
    if (result.candidates.empty()) {
        return;
    }
    // Only fresh, usable network results enter the denominator. Account before
    // insertion, so neither our own candidates nor later cache/UI replays can
    // be mistaken for native matches.
    compareNativeCandidates(ic, result.candidates);
    rememberCandidates(cacheKey(result.pinyin, snapshot.context),
                       result.candidates);
    insertCandidates(ic, state, state->candidates);
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
    const std::string badge =
        config_.showBadge.value() ? config_.badgeSymbol.value() : std::string();

    // Tell the user where the dictionary and the model agree. This happens
    // before the early returns below: an all-overlap reply inserts nothing, and
    // that is precisely when the mark is the only visible sign of the model.
    // Switching the option off also takes the marks back on the next refresh.
    {
        const bool wanted = config_.showOverlapBadge.value();
        const std::string symbol = config_.overlapSymbol.value();
        bool changed = false;
        for (int i = 0; i < total; ++i) {
            const auto &word = modifiable->candidateFromAll(i);
            if (dynamic_cast<const LlmCandidateWord *>(&word)) {
                continue;
            }
            const bool agrees =
                wanted &&
                std::ranges::find(candidates, word.text().toString()) !=
                    candidates.end();
            // A symbol that was changed in the settings is taken back before
            // the new one is written, so two marks never stack up.
            if (!agrees || appliedOverlapSymbol_ != symbol) {
                changed |= unmarkOverlap(word, appliedOverlapSymbol_);
            }
            if (agrees) {
                changed |= markOverlap(word, symbol);
            }
        }
        appliedOverlapSymbol_ = symbol;
        if (changed) {
            state->inserting = true;
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            state->inserting = false;
        }
    }

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

    const auto valid =
        [owner = watch(), ref = ic->watch(),
         snapshot = InputSnapshot{state->generation, state->pinyin,
                                  state->context, state->selected}](InputContext *selectedIc) {
            auto *self = owner.get();
            auto *ic = ref.get();
            return self && ic && selectedIc == ic &&
                   self->matchesInput(ic, snapshot);
        };
    state->inserting = true;
    // Insert back to front so the first candidate ends up first on screen.
    for (int i = static_cast<int>(toInsert.size()) - 1; i >= 0; i--) {
        auto adopted = [owner = watch(), id = state->historyId,
                        text = toInsert[i]] {
            if (auto *self = owner.get();
                self && self->config_.recordHistory.value()) {
                self->history_.adopt(id, text, false);
                self->refreshHistory();
            }
        };
        modifiable->insert(position,
                           std::make_unique<LlmCandidateWord>(
                               toInsert[i], state->selected, badge, valid,
                               std::move(adopted)));
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
        if (isLlmInputMethod(ic) &&
            (ic->hasFocus() || ic == instance_->mostRecentInputContext()) &&
            ic->statusArea().isChild(&toggleAction_)) {
            // StatusArea refresh also republishes the AI/PY input method label.
            // Do not rebuild GNOME's status menu on every keystroke.
            toggleAction_.update(ic);
        }
        return true;
    });
}

void ChineseLlm::openHistory(bool launchBrowser) {
    if (!writeHistorySnapshot()) {
        FCITX_WARN()
            << "chinese-llm: unable to create private history snapshot";
        return;
    }
    if (launchBrowser) {
        // Argument vector, never a shell command or model-controlled URL.
        startProcess({"xdg-open", historySnapshot_.path()});
    }
}

void ChineseLlm::clearHistory() {
    historySnapshotDirty_ = false;
    historySnapshotTimer_.reset();
    history_.clear();
    historySnapshot_.clear();
    instance_->inputContextManager().foreach([this](InputContext *ic) {
        auto *state = ic->propertyFor(&factory_);
        state->historyId = 0;
        state->commitHistoryId = 0;
        state->commitSelected.clear();
        return true;
    });
    refreshHistory();
}

bool ChineseLlm::writeHistorySnapshot() {
    historySnapshotDirty_ = false;
    return historySnapshot_.write(
        renderHistoryHtml(history_, config_.recordHistory.value(),
                          historyLanguageFromEnvironment()));
}

void ChineseLlm::scheduleHistorySnapshot() {
    // Rendering up to 100 rows and rewriting the file on every request event
    // would run on the input thread several times per keystroke. An opened
    // snapshot only needs to be current by the time the user refreshes it, so
    // changes within a short window share one trailing rewrite.
    if (historySnapshotDirty_ || !historySnapshot_.exists()) {
        return;
    }
    historySnapshotDirty_ = true;
    historySnapshotTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kSnapshotDelayUs,
        10 * 1000, [this](fcitx::EventSourceTime *, uint64_t) {
            if (historySnapshotDirty_ && !writeHistorySnapshot()) {
                FCITX_WARN()
                    << "chinese-llm: unable to update private history snapshot";
            }
            return true;
        });
    historySnapshotTimer_->setOneShot();
}

void ChineseLlm::refreshHistory() {
    const auto count = history_.entries().size();
    const auto text = std::vformat(D_("chinese-llm", "LLM request history {0}"),
                                   std::make_format_args(count));
    const bool changed = historyAction_.shortText(nullptr) != text;
    historyAction_.setShortText(text);
    scheduleHistorySnapshot();
    if (changed) {
        instance_->inputContextManager().foreach([this](InputContext *ic) {
            if (isLlmInputMethod(ic) &&
                (ic->hasFocus() || ic == instance_->mostRecentInputContext()) &&
                ic->statusArea().isChild(&historyAction_)) {
                historyAction_.update(ic);
            }
            return true;
        });
    }
}

bool ChineseLlm::overlapsNative(InputContext *ic,
                                const std::vector<std::string> &candidates) {
    const auto list = ic->inputPanel().candidateList();
    auto *native = list ? list->toModifiable() : nullptr;
    if (!native) {
        return false;
    }
    for (int i = 0; i < native->totalSize(); ++i) {
        const auto &word = native->candidateFromAll(i);
        if (!dynamic_cast<const LlmCandidateWord *>(&word) &&
            std::ranges::find(candidates, word.text().toString()) !=
                candidates.end()) {
            return true;
        }
    }
    return false;
}

void ChineseLlm::compareNativeCandidates(
    InputContext *ic, const std::vector<std::string> &candidates) {
    const auto list = ic->inputPanel().candidateList();
    auto *native = list ? list->toModifiable() : nullptr;
    if (!native || candidates.empty()) {
        return;
    }
    std::unordered_set<std::string> nativeTexts;
    nativeTexts.reserve(native->totalSize());
    for (int i = 0; i < native->totalSize(); ++i) {
        const auto &candidate = native->candidateFromAll(i);
        if (!dynamic_cast<const LlmCandidateWord *>(&candidate)) {
            nativeTexts.insert(candidate.text().toString());
        }
    }
    if (nativeTexts.empty()) {
        return;
    }
    ++comparedRequests_;
    // This is request-level any-match overlap, not top-1 agreement or the
    // number of matching candidates. Compare all sanitized output, regardless
    // of display position/limit/badge. Temporary texts never leave this call.
    if (std::ranges::any_of(candidates, [&nativeTexts](const auto &candidate) {
            return nativeTexts.contains(candidate);
        })) {
        ++overlappingRequests_;
    }
    refreshStatistics();
}

void ChineseLlm::refreshStatistics() {
    auto statistics = requestStatistics();
    // Standard formatting is locale-independent here: no thousands separators
    // or ASCII ':' can accidentally enter the Kimpanel property protocol.
    const auto rate =
        statistics.compared
            ? std::format("{:.1f}%",
                          100.0 * static_cast<double>(statistics.overlapping) /
                              static_cast<double>(statistics.compared))
            : std::string("—");
    const std::array<std::pair<SimpleAction *, std::string>, 3> rows{{
        {&requestsAction_,
         std::vformat(
             D_("chinese-llm", "LLM requests {0} (valid {1})"),
             std::make_format_args(statistics.sent, statistics.compared))},
        {&overlapAction_,
         std::vformat(D_("chinese-llm", "LLM overlapping requests {0}"),
                      std::make_format_args(statistics.overlapping))},
        {&rateAction_, std::vformat(D_("chinese-llm", "LLM overlap rate {0}"),
                                    std::make_format_args(rate))},
    }};
    std::vector<SimpleAction *> changed;
    for (const auto &[action, text] : rows) {
        if (action->shortText(nullptr) != text) {
            action->setShortText(text);
            changed.push_back(action);
        }
    }
    if (changed.empty()) {
        return;
    }
    instance_->inputContextManager().foreach(
        [this, &changed](InputContext *ic) {
            // Kimpanel also accepts its old, unfocused lastInputContext_.
            // Never let a late start notification republish a dormant IM's
            // menu over another window. mostRecent preserves shell menus.
            if (isLlmInputMethod(ic) &&
                (ic->hasFocus() || ic == instance_->mostRecentInputContext())) {
                for (auto *action : changed) {
                    if (ic->statusArea().isChild(action)) {
                        action->update(ic);
                    }
                }
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
