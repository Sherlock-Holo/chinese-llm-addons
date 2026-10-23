/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "chinese-llm.h"
#include "llmbackend.h"
#include "llmworker.h"
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
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <string>
#include <utility>
#include <vector>

namespace fcitx::chinesellm {

namespace {

/// The one definition of the log category. llmworker.cpp only declares it, so
/// the same category name is used without a duplicate definition.
FCITX_DEFINE_LOG_CATEGORY(chinesellm, "chinese-llm");

constexpr char kEnvApiKey[] = "FCITX_CHINESE_LLM_API_KEY";
/// Suffix appended to a candidate's comment so the user can tell model output
/// from dictionary output at a glance.
constexpr char kBadge[] = "LLM";

/**
 * A candidate backed by model output.
 *
 * Selecting it commits the text, clears the input panel, and resets the input
 * context. The reset is what makes the pinyin engine drop its composition:
 * Instance watches InputContextReset in the InputMethod phase and calls
 * PinyinEngine::reset(), which clears the buffer and rebuilds the panel. Without
 * it the engine would keep the half-typed pinyin after our text went out.
 */
class LlmCandidateWord : public fcitx::CandidateWord {
public:
    LlmCandidateWord(std::string text, bool badge) : text_(std::move(text)) {
        setText(fcitx::Text(text_));
        if (badge) {
            fcitx::Text comment(kBadge);
            setComment(comment);
        }
    }

    void select(fcitx::InputContext *inputContext) const override {
        inputContext->commitString(text_);
        inputContext->inputPanel().reset();
        inputContext->reset();
    }

private:
    std::string text_;
};

} // namespace

ChineseLlm::ChineseLlm(fcitx::AddonManager *manager)
    : worker_(std::make_unique<LlmWorker>()),
      instance_(manager->instance()),
      dispatcher_(manager->instance()->eventDispatcher()) {
    instance_->inputContextManager().registerProperty("chinese-llm", &factory_);

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

    worker_->start();
    FCITX_INFO() << "chinese-llm: loaded, endpoint " << config_.endpoint.value();
}

ChineseLlm::~ChineseLlm() { worker_->stop(); }

void ChineseLlm::reloadConfig() {
    readAsIni(config_, "conf/chinese-llm.conf");
    // Any in-flight reply was produced under the old settings; drop it rather
    // than mix output from two different models in one candidate list.
    if (worker_) {
        worker_->cancelAll();
    }
}

void ChineseLlm::setConfig(const fcitx::RawConfig &config) {
    config_.load(config, true);
    fcitx::safeSaveAsIni(config_, "conf/chinese-llm.conf");
    if (worker_) {
        worker_->cancelAll();
    }
}

void ChineseLlm::save() {
    fcitx::safeSaveAsIni(config_, "conf/chinese-llm.conf");
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

bool ChineseLlm::isEligible(fcitx::InputContext *ic) {
    if (!config_.enabled.value()) {
        return false;
    }
    // Never look at a password field, even to read the preedit.
    if (ic->capabilityFlags().testAny(
            fcitx::CapabilityFlag::PasswordOrSensitive)) {
        return false;
    }
    const auto *entry = instance_->inputMethodEntry(ic);
    if (!entry || entry->addon() != "pinyin") {
        return false;
    }
    if (config_.skipShuangpin.value() && entry->uniqueName() == "shuangpin") {
        return false;
    }
    auto candidateList = ic->inputPanel().candidateList();
    if (!candidateList || candidateList->empty()) {
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
    const size_t usable =
        std::min<size_t>(cursor, static_cast<size_t>(limit));
    // Start `usable` characters before the cursor. ncharByteLength on a char
    // iterator walks from that many characters *after* the cursor, so measure
    // from the beginning of the string instead.
    const auto startChar = std::next(fcitx::utf8::MakeUTF8CharRange(text).begin(),
                                     static_cast<ptrdiff_t>(cursor - usable));
    const auto endChar = std::next(
        fcitx::utf8::MakeUTF8CharRange(text).begin(),
        static_cast<ptrdiff_t>(cursor));
    return std::string(startChar.view().data(), startChar.view().size()) +
           std::string(endChar.view().data(), endChar.view().size());
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
        return;
    }

    // The pinyin being typed comes from one of two places depending on whether
    // the client advertises the Preedit capability. In ComposingPinyin mode the
    // engine fills clientPreedit; otherwise (PreeditMode::No, which is what a
    // client without the flag gets) it only fills the server side preedit. See
    // PinyinEngine::preedit() in fcitx5-chinese-addons. Reading only the client
    // one silently does nothing in any client that does not support preedit.
    auto preedit = ic->inputPanel().clientPreedit().toString();
    if (preedit.empty()) {
        preedit = ic->inputPanel().preedit().toString();
    }
    // Once the user selects a character the preedit becomes a Han/pinyin mix and
    // we step away, because the public API offers no reliable way to split it.
    if (!isPlainPinyin(preedit)) {
        return;
    }
    std::string pinyin = preedit;
    std::replace(pinyin.begin(), pinyin.end(), ' ', '\'');
    if (pinyin.size() > MAX_PINYIN_LENGTH) {
        return;
    }
    // The length is compared against characters, not syllables, because the
    // separator form varies with the engine's segmentation.
    if (static_cast<int>(utf8::length(pinyin)) <
        config_.minPinyinLength.value()) {
        return;
    }
    if (state->pinyin == pinyin) {
        return;
    }
    state->pinyin = pinyin;
    state->generation++;

    // A cache hit can be shown without a round trip and without debouncing.
    if (const auto iter = cache_.find(pinyin); iter != cache_.end()) {
        insertCandidates(ic, state, iter->second);
        return;
    }

    scheduleRequest(ic, state, std::move(pinyin), buildContext(ic));
}

void ChineseLlm::scheduleRequest(fcitx::InputContext *ic, LlmState *state,
                                 std::string pinyin, std::string context) {
    const int debounce = config_.debounceMs.value();
    if (debounce <= 0) {
        dispatchRequest(ic, state, std::move(pinyin), std::move(context),
                        state->generation);
        return;
    }

    // The debounce timer lives per input context and is restarted on every
    // keystroke, so a burst of typing produces exactly one request. Doing this
    // here rather than in the worker matters: once a request has started, the
    // worker cannot abandon it, and a local model takes long enough that a
    // started request is already wasted.
    state->debounce = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        fcitx::now(CLOCK_MONOTONIC) + static_cast<uint64_t>(debounce) * 1000,
        static_cast<uint64_t>(debounce) * 1000,
        [this, ref = ic->watch(), pinyin = std::move(pinyin),
         context = std::move(context)](fcitx::EventSourceTime *,
                                       uint64_t) mutable {
            auto *inputContext = ref.get();
            if (!inputContext) {
                return true;
            }
            auto *cur = inputContext->propertyFor(&factory_);
            dispatchRequest(inputContext, cur, std::move(pinyin),
                            std::move(context), cur->generation);
            return true;
        });
    state->debounce->setOneShot();
}

void ChineseLlm::dispatchRequest(fcitx::InputContext *ic, LlmState *state,
                                 std::string pinyin, std::string context,
                                 uint64_t generation) {
    // Take the weak reference here, on the main thread. Creating one inside the
    // worker would touch the input context from the wrong thread.
    auto ref = ic->watch();
    worker_->submit(
        backendConfig(), std::move(pinyin), std::move(context),
        [this, ref = std::move(ref),
         generation](FetchResult result) {
            dispatcher_.scheduleWithContext(ref, [this, ref, generation,
                                                  result = std::move(result)]() {
                auto *inputContext = ref.get();
                if (!inputContext) {
                    return;
                }
                auto *state = inputContext->propertyFor(&factory_);
                if (state->generation != generation) {
                    // The user typed on; this reply is about an older pinyin.
                    return;
                }
                if (!result.ok) {
                    noteError(result.error);
                    return;
                }
                if (result.candidates.empty()) {
                    // A well-formed reply that sanitize rejected is not an
                    // endpoint failure, so it does not count towards cooldown.
                    return;
                }
                errorCount_ = 0;
                rememberCandidates(result.pinyin, result.candidates);
                insertCandidates(inputContext, state, result.candidates);
            });
        });
}

void ChineseLlm::rememberCandidates(const std::string &pinyin,
                                    const std::vector<std::string> &candidates) {
    const int cacheSize = config_.cacheSize.value();
    if (cacheSize <= 0) {
        return;
    }
    if (cache_.find(pinyin) != cache_.end()) {
        return;
    }
    cache_.emplace(pinyin, candidates);
    cacheOrder_.push_back(pinyin);
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

    state->inserting = true;
    // Insert back to front so the first candidate ends up first on screen.
    for (int i = static_cast<int>(toInsert.size()) - 1; i >= 0; i--) {
        modifiable->insert(position,
                           std::make_unique<LlmCandidateWord>(toInsert[i], badge));
    }
    state->inserting = false;

    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void ChineseLlm::handleKeyEvent(fcitx::Event &event) {
    auto &keyEvent = static_cast<fcitx::KeyEvent &>(event);
    if (keyEvent.isRelease()) {
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
    if (!config_.enabled.value()) {
        worker_->cancelAll();
        errorCount_ = 0;
        cooldownUntil_ = std::chrono::steady_clock::time_point{};
    }
    FCITX_INFO() << "chinese-llm: "
                 << (config_.enabled.value() ? "enabled" : "disabled");
}

void ChineseLlm::noteError(const std::string &error) {
    errorCount_++;
    FCITX_DEBUG() << "chinese-llm: request failed (" << errorCount_
                  << "): " << error;
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
    cooldownUntil_ = std::chrono::steady_clock::time_point{};
    errorCount_ = 0;
}

} // namespace fcitx::chinesellm

// The loader derives the V2 symbol name from the addon name, which here is
// "chinese-llm" -- a hyphen is not a valid C identifier, so that symbol can
// never exist. It then falls back to the unsuffixed
// "fcitx_addon_factory_instance", which is why the backwards-compatible macro
// is required rather than optional.
FCITX_ADDON_FACTORY_V2_BACKWARDS(chinese_llm,
                                 fcitx::chinesellm::ChineseLlmFactory);
