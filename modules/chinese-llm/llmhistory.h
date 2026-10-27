/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef FCITX_CHINESE_LLM_LLMHISTORY_H
#define FCITX_CHINESE_LLM_LLMHISTORY_H

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "llmworker.h"

namespace fcitx::chinesellm {

struct RequestHistoryEntry {
    uint64_t id = 0;
    uint64_t seq = 0;
    int64_t timestampMs = 0;
    std::string pinyin;
    std::string model;
    std::string status = "queued";
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
// IDs never reset, including after clear(), so late callbacks cannot resurrect
// a cleared or evicted entry. No request context, API key or request body is kept.
class RequestHistory {
public:
    ~RequestHistory();
    uint64_t begin(std::string pinyin, std::string model);
    RequestHistoryEntry *find(uint64_t id);
    const RequestHistoryEntry *find(uint64_t id) const;
    void complete(uint64_t id, const FetchResult &result);
    void adopt(uint64_t id, std::string text, bool native);
    void clear();
    const std::deque<RequestHistoryEntry> &entries() const { return entries_; }

private:
    uint64_t nextId_ = 0;
    std::deque<RequestHistoryEntry> entries_;
    // Exact original identities, never used for rendering. All nonempty
    // candidates may be retained, independently of the 32-item display cap,
    // with at most 64 KiB of text per entry (the worker's response ceiling).
    // Rejecting empty identities also bounds the number of stored strings.
    std::unordered_map<uint64_t, std::vector<std::string>> candidateIdentities_;
};

// Standalone, read-only, script-free HTML. All input is treated as untrusted.
std::string renderHistoryHtml(const RequestHistory &history, bool enabled);

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
        return directory_.empty() ? std::string() : directory_ + "/history.html";
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
