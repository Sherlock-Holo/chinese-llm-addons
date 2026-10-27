/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef _CHINESE_LLM_LLMWORKER_H_
#define _CHINESE_LLM_LLMWORKER_H_

#include "llmbackend.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <curl/curl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fcitx::chinesellm {

/// Result of one request. The callback runs on the worker thread; the caller
/// must marshal it to the main thread and revalidate seq before using it.
struct FetchResult {
    uint64_t seq = 0;
    /// True when the request produced a valid, complete chat response. A false
    /// value still carries the sequence number so a superseded request can be
    /// discarded without being counted as an error.
    bool ok = false;
    std::string pinyin;
    std::string error;
    std::vector<std::string> candidates;
};

using FetchCallback = std::function<void(FetchResult)>;

/// A single background thread that owns one curl easy handle.
///
/// This deliberately is not curl_multi: the pinyin engine would fire one
/// request per keystroke, and the fix for that is to not send them, not to
/// multiplex them. One request at a time plus supersede gives predictable
/// latency and keeps a runaway local model from piling up work.
class LlmWorker {
public:
    LlmWorker();
    ~LlmWorker();

    LlmWorker(const LlmWorker &) = delete;
    LlmWorker &operator=(const LlmWorker &) = delete;

    void start();
    void stop();

    /// Queue a request, replacing any request not yet started.
    ///
    /// The sequence number identifies the newest request; a response whose
    /// sequence is older than the newest one is dropped by the caller.
    /// started runs on the worker thread when a logical transfer is attempted,
    /// even if its reply is later cancelled or superseded. Marshal UI work to
    /// the main thread; neither callback may throw.
    uint64_t submit(BackendConfig config, std::string pinyin,
                    std::string context, FetchCallback callback,
                    std::function<void()> started = {});

    /// Logical network attempts, not queued jobs or redirect hops. Includes
    /// failed/cancelled transfers; starting curl does not prove server receipt.
    /// Kept for this worker's lifetime, including stop/start and cancelAll().
    uint64_t requestsStarted() const {
        return requestsStarted_.load(std::memory_order_relaxed);
    }

    /// Invalidate every queued and in-flight request. Called when the config
    /// changes or the addon is toggled, so stale replies cannot land.
    void cancelAll();

    /// Latest sequence, including invalidation by cancelAll() or stop().
    uint64_t currentSeq() const { return seq_.load(std::memory_order_relaxed); }

private:
    struct Job {
        uint64_t seq = 0;
        BackendConfig config;
        std::string pinyin;
        std::string context;
        FetchCallback callback;
        std::function<void()> started;
    };

    void run();
    /// Perform one request. Returns false on a transport or HTTP failure.
    bool perform(const Job &job, std::string &body, std::string &error);
    bool isCurrent(uint64_t seq) const;

    static size_t writeCallback(char *ptr, size_t size, size_t nmemb,
                                void *userdata);
    struct TransferState {
        const LlmWorker *worker;
        uint64_t seq;
    };
    static int progressCallback(void *userdata, curl_off_t, curl_off_t,
                                curl_off_t, curl_off_t);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> requestsStarted_{0};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Job> pending_;

    // Owned by the worker thread once started; created before the thread so
    // construction failures surface on the main thread.
    CURL *curl_ = nullptr;
};

} // namespace fcitx::chinesellm

#endif // _CHINESE_LLM_LLMWORKER_H_
