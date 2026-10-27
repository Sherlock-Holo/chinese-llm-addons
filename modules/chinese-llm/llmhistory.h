/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef FCITX_CHINESE_LLM_LLMHISTORY_H
#define FCITX_CHINESE_LLM_LLMHISTORY_H

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "llmworker.h"

namespace fcitx::chinesellm {

enum class RequestStatus {
    Queued,
    InFlight,
    CacheHit,
    Cancelled,
    Error,
    Empty,
    Filtered,
    /// Finished with accepted candidates; not yet known to be shown.
    Completed,
    /// Fresh, current and shown to the user.
    Usable,
    /// Finished, but the input had moved on.
    Stale,
};

/// Stable lowercase identifier, for logs and tests. Inline: the addon test
/// executable does not link the runtime module.
inline const char *statusName(RequestStatus status) {
    switch (status) {
    case RequestStatus::Queued:
        return "queued";
    case RequestStatus::InFlight:
        return "inflight";
    case RequestStatus::CacheHit:
        return "cache_hit";
    case RequestStatus::Cancelled:
        return "cancelled";
    case RequestStatus::Error:
        return "error";
    case RequestStatus::Empty:
        return "empty";
    case RequestStatus::Filtered:
        return "filtered";
    case RequestStatus::Completed:
        return "completed";
    case RequestStatus::Usable:
        return "usable";
    case RequestStatus::Stale:
        return "stale";
    }
    return "";
}

struct RequestHistoryEntry {
    uint64_t id = 0;
    uint64_t seq = 0;
    int64_t timestampMs = 0;
    std::string pinyin;
    std::string model;
    RequestStatus status = RequestStatus::Queued;
    std::string content;
    std::string error;
    std::string adoption;
    std::string adoptedText;
    std::vector<std::string> candidates;
    std::vector<RejectedCandidate> rejected;
    bool started = false;
    uint64_t elapsedMs = 0;
    long httpStatus = 0;
    std::optional<bool> overlap;
};

// Main-thread only. The deque is oldest first and retains at most 100 entries.
// IDs never reset, including after clear(), so late events cannot resurrect a
// cleared or evicted entry. No request context, API key or request body is
// kept.
//
// Entries are only changed through the lifecycle methods below, which keep the
// status transitions in one place. Every method ignores an unknown id.
class RequestHistory {
public:
    ~RequestHistory();
    /// A request has been queued for the worker. Returns 0 if ids ran out.
    uint64_t begin(std::string pinyin, std::string model);
    /// A reply was served from the cache; no network request was made.
    uint64_t recordCacheHit(std::string pinyin, std::string model,
                            const std::vector<std::string> &candidates,
                            bool overlap);
    /// The worker began a network transfer for this request.
    void markStarted(uint64_t id);
    /// The worker sequence number the request was submitted with.
    void setSeq(uint64_t id, uint64_t seq);
    /// The input changed or the feature was switched off while queued or in
    /// flight. A later finish() keeps this reason.
    void markCancelled(uint64_t id, std::string reason);
    /// Record the worker's final outcome, including diagnostics.
    void finish(uint64_t id, const FetchResult &result);
    /// The finished reply is current and its candidates were shown.
    void markUsable(uint64_t id, bool overlap);
    /// The finished reply no longer matches the input and was discarded.
    void markStale(uint64_t id);
    void adopt(uint64_t id, std::string text, bool native);
    /// True if committing one of the entry's candidates would still be
    /// recorded as its adoption (shown, and not adopted yet).
    bool adoptable(uint64_t id) const;
    void clear();

    const RequestHistoryEntry *find(uint64_t id) const;
    const std::deque<RequestHistoryEntry> &entries() const { return entries_; }

private:
    RequestHistoryEntry *mutableEntry(uint64_t id);
    void setIdentities(uint64_t id, const std::vector<std::string> &candidates);
    uint64_t nextId_ = 0;
    std::deque<RequestHistoryEntry> entries_;
    // Exact original identities, never used for rendering. All nonempty
    // candidates may be retained, independently of the 32-item display cap,
    // with at most 64 KiB of text per entry (the worker's response ceiling).
    // Rejecting empty identities also bounds the number of stored strings.
    std::unordered_map<uint64_t, std::vector<std::string>> candidateIdentities_;
};

/// The page is written in one language, never both. Chinese is the default and
/// is used unless the user's locale names another language.
enum class HistoryLanguage { Chinese, English };
HistoryLanguage historyLanguageFromLocale(std::string_view locale);
HistoryLanguage historyLanguageFromEnvironment();

// Standalone, read-only, script-free HTML. All input is treated as untrusted.
std::string renderHistoryHtml(const RequestHistory &history, bool enabled,
                              HistoryLanguage language = HistoryLanguage::Chinese);

// No filesystem work until an explicit write(). Each snapshot is a 0600 file
// in a private 0700 directory. write() atomically replaces the old snapshot;
// clear() and destruction remove only paths owned by this instance.
class HistorySnapshot {
public:
    HistorySnapshot() = default;
    ~HistorySnapshot();
    HistorySnapshot(const HistorySnapshot &) = delete;
    HistorySnapshot &operator=(const HistorySnapshot &) = delete;
    HistorySnapshot(HistorySnapshot &&) = delete;
    HistorySnapshot &operator=(HistorySnapshot &&) = delete;

    bool write(const std::string &html);
    std::string path() const {
        return directory_.empty() ? std::string()
                                  : directory_ + "/history.html";
    }
    bool exists() const;
    void clear();

private:
    bool ensureDirectory();
    std::string directory_;
    int directoryFd_ = -1;
    uint64_t temporaryId_ = 0;
};

} // namespace fcitx::chinesellm

#endif // FCITX_CHINESE_LLM_LLMHISTORY_H
