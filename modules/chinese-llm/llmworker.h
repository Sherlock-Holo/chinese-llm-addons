/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef _CHINESE_LLM_LLMWORKER_H_
#define _CHINESE_LLM_LLMWORKER_H_

#include "llmbackend.h"
#include <atomic>
#include <chrono>
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

/// Result of one request. Handlers run off the main thread; the caller must
/// marshal the result to the main thread and decide there whether it is still
/// current (compare seq with LlmWorker::currentSeq() and its own input key).
struct FetchResult {
    uint64_t seq = 0;
    /// True when the request produced a valid, complete chat response. A false
    /// value still carries the sequence number so a superseded request can be
    /// discarded without being counted as an error.
    bool ok = false;
    /// An actual curl attempt, not merely a submitted/processed job.
    bool started = false;
    /// Superseded, invalidated or stopped. Diagnostics may still be present.
    bool cancelled = false;
    /// Logical transfer start through parsing/sanitizing completion, in ms.
    uint64_t elapsedMs = 0;
    /// Captured even for HTTP/transport errors; 0 means no HTTP response.
    long httpStatus = 0;
    std::string pinyin;
    std::string error;
    /// Raw extracted message.content, not the request or response envelope.
    std::string content;
    std::vector<std::string> candidates;
    /// Sanitizer diagnostics, or incomplete_response for a non-stop finish.
    std::vector<RejectedCandidate> rejected;
};

/// Lifecycle notifications for one submitted job.
enum class FetchStage {
    /// A logical transfer is about to begin (curl is started).
    Started,
    /// Delivered exactly once for every submitted job: completed, failed,
    /// cancelled mid-transfer, or cancelled/replaced before it ever started.
    Finished,
};

/// May run on the worker thread or on the thread that called submit(),
/// cancelAll() or stop() (for jobs dropped before they start). It must not
/// throw and must only marshal work to the main thread. Whether a Finished
/// result is still current is the caller's decision (see FetchResult::seq and
/// FetchResult::cancelled).
using FetchHandler = std::function<void(FetchStage, const FetchResult &)>;

/// A single background thread that owns one curl easy handle.
///
/// This deliberately does not run requests in parallel: the pinyin engine
/// would fire one request per keystroke, and the fix for that is to not send
/// them, not to multiplex them. One request at a time plus supersede gives
/// predictable latency and keeps a runaway local model from piling up work.
/// The easy handle is driven through a curl_multi handle only so that
/// cancellation can wake the transfer immediately; a plain easy transfer only
/// notices cancellation at its next progress tick, about once per second.
class LlmWorker {
public:
    LlmWorker();
    ~LlmWorker();

    LlmWorker(const LlmWorker &) = delete;
    LlmWorker &operator=(const LlmWorker &) = delete;

    void start();
    void stop();

    /// Queue a request, replacing any request not yet started, and abort any
    /// transfer in flight (it can no longer be the newest). The replaced
    /// queued job is reported as cancelled through its own handler.
    ///
    /// The sequence number identifies the newest request. Every job gets one
    /// Finished event; Started precedes it only if a transfer was attempted.
    uint64_t submit(BackendConfig config, std::string pinyin,
                    std::string context, FetchHandler handler);

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
        FetchHandler handler;
    };

    void run();
    /// Perform one request. Returns false on a transport or HTTP failure.
    bool
    perform(const Job &job, std::string &body, FetchResult &result,
            std::optional<std::chrono::steady_clock::time_point> &startedAt);
    bool isCurrent(uint64_t seq) const;
    /// Report a job that never reached curl as cancelled. Must be called
    /// without mutex_ held: the handler is arbitrary caller code.
    static void reportDropped(Job &job);
    /// Run the prepared easy handle to completion or until seq stops being
    /// current.
    CURLcode transfer(uint64_t seq);
    /// Wake a transfer blocked in curl_multi_poll so it re-checks isCurrent().
    void wake();

    static size_t writeCallback(char *ptr, size_t size, size_t nmemb,
                                void *userdata);

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
    CURLM *multi_ = nullptr;
};

} // namespace fcitx::chinesellm

#endif // _CHINESE_LLM_LLMWORKER_H_
