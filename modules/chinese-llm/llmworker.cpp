/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "llmworker.h"
#include "llmbackend.h"
#include <algorithm>
#include <chrono>
#include <curl/curl.h>
#include <fcitx-utils/log.h>
#include <stdexcept>
#include <utility>

namespace fcitx::chinesellm {

namespace {

struct WriteSink {
    std::string data;
    bool overflow = false;
};

} // namespace

LlmWorker::LlmWorker() {
    curl_ = curl_easy_init();
    if (!curl_) {
        throw std::runtime_error("Failed to init CURL handle.");
    }
    // These are safe to set once and never change. The body pointer is set per
    // request because curl does not copy CURLOPT_POSTFIELDS.
    const bool ok =
        (curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION,
                          &LlmWorker::writeCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L) == CURLE_OK) &&
        // Follow a redirect so an endpoint behind a proxy works, but never
        // leak the Authorization header to another host.
        (curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK) &&
        (curl_easy_setopt(curl_, CURLOPT_MAXREDIRS, 3L) == CURLE_OK) &&
        // Chat completions remain POSTs on 301/302/303; curl otherwise changes
        // these into GETs and silently drops the JSON request body.
        (curl_easy_setopt(curl_, CURLOPT_POSTREDIR,
                          static_cast<long>(CURL_REDIR_POST_ALL)) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl_, CURLOPT_UNRESTRICTED_AUTH, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION,
                          &LlmWorker::progressCallback) == CURLE_OK);
    if (!ok) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
        throw std::runtime_error("Failed to setup CURL handle options.");
    }
}

LlmWorker::~LlmWorker() {
    stop();
    if (curl_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }
}

void LlmWorker::start() {
    if (thread_.joinable()) {
        return;
    }
    stop_.store(false, std::memory_order_relaxed);
    thread_ = std::thread(&LlmWorker::run, this);
}

void LlmWorker::stop() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stop_.store(true, std::memory_order_relaxed);
        seq_.fetch_add(1, std::memory_order_relaxed);
        pending_.reset();
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

uint64_t LlmWorker::submit(BackendConfig config, std::string pinyin,
                           std::string context, FetchCallback callback) {
    uint64_t seq;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // Assign the sequence under the same lock as pending_, so cancellation
        // and concurrent submissions cannot replace a newer job with an old
        // one.
        seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        // Replacing rather than queuing is the whole point: only the newest
        // pinyin is worth asking about, and this is where the per-keystroke
        // fan-out is collapsed into a single request.
        pending_.emplace(Job{.seq = seq,
                             .config = std::move(config),
                             .pinyin = std::move(pinyin),
                             .context = std::move(context),
                             .callback = std::move(callback)});
    }
    cv_.notify_one();
    return seq;
}

void LlmWorker::cancelAll() {
    // Bumping the sequence makes replies stale and the progress callback aborts
    // an in-flight transfer, freeing the single worker for the next request.
    const std::lock_guard<std::mutex> lock(mutex_);
    seq_.fetch_add(1, std::memory_order_relaxed);
    pending_.reset();
}

bool LlmWorker::isCurrent(uint64_t seq) const {
    return !stop_.load(std::memory_order_relaxed) &&
           seq == seq_.load(std::memory_order_relaxed);
}

void LlmWorker::run() {
    while (true) {
        std::optional<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,
                     [this] { return stop_.load() || pending_.has_value(); });
            if (stop_.load()) {
                return;
            }
            job = std::move(pending_);
            pending_.reset();
        }
        if (!job) {
            continue;
        }

        FetchResult result;
        result.seq = job->seq;
        result.pinyin = job->pinyin;
        try {
            std::string body;
            result.ok = perform(*job, body, result.error);
            if (result.ok && isCurrent(job->seq)) {
                std::string content;
                if (parseResponse(body, content)) {
                    result.candidates = sanitize(content, result.pinyin, 0);
                } else {
                    result.ok = false;
                    result.error = "invalid completion response";
                }
            }
        } catch (const std::exception &e) {
            // Invalid UTF-8 in configuration can make JSON serialization throw.
            // Report request processing failures, never terminate the worker
            // (and thus the entire input method). Callback exceptions remain
            // the caller's responsibility and are deliberately not caught here.
            result.ok = false;
            result.candidates.clear();
            result.error =
                std::string("request processing failed: ") + e.what();
        }
        if (!isCurrent(job->seq)) {
            FCITX_DEBUG() << "chinese-llm: discarding superseded reply for "
                          << result.pinyin;
            continue;
        }
        // Parsing/sanitizing may take time. Check again immediately before
        // delivery; the main-thread consumer must also validate result.seq,
        // since a request can be superseded after this check or while queued.
        if (job->callback && isCurrent(job->seq)) {
            job->callback(std::move(result));
        }
    }
}

bool LlmWorker::perform(const Job &job, std::string &body, std::string &error) {
    if (!isCurrent(job.seq)) {
        error = "cancelled";
        return false;
    }
    const auto spec = buildRequest(job.config, job.pinyin, job.context);

    curl_slist *headers = nullptr;
    for (const auto &header : spec.headers) {
        auto *appended = curl_slist_append(headers, header.c_str());
        if (!appended) {
            curl_slist_free_all(headers);
            error = "failed to allocate request headers";
            return false;
        }
        headers = appended;
    }

    WriteSink sink;
    TransferState transfer{this, job.seq};
    // The timeout must be per request: a shorter budget is deliberately used
    // here than cloudpinyin's hardcoded 10s because a slow reply is useless
    // once the user has typed past it.
    const long timeout = std::clamp<long>(
        job.config.timeoutSeconds > 0 ? job.config.timeoutSeconds : 5, 1, 60);
    const long connectTimeout = std::min<long>(timeout, 3);

    bool ok = false;
    do {
        if (curl_easy_setopt(curl_, CURLOPT_URL, spec.url.c_str()) !=
            CURLE_OK) {
            error = "invalid url";
            break;
        }
        if (curl_easy_setopt(curl_, CURLOPT_TIMEOUT, timeout) != CURLE_OK ||
            curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, connectTimeout) !=
                CURLE_OK) {
            error = "invalid timeout";
            break;
        }
        // These three must be set on every request: the handle is reused and a
        // previous POST would otherwise leak its body into a plain GET.
        if (curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
            curl_easy_setopt(curl_, CURLOPT_POST, 1L) != CURLE_OK ||
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, spec.body.c_str()) !=
                CURLE_OK ||
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(spec.body.size())) != CURLE_OK) {
            error = "invalid post body";
            break;
        }
        if (curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &sink) != CURLE_OK ||
            curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, &transfer) !=
                CURLE_OK) {
            error = "invalid transfer callbacks";
            break;
        }
        if (!isCurrent(job.seq)) {
            error = "cancelled";
            break;
        }

        const auto code = curl_easy_perform(curl_);
        if (code != CURLE_OK) {
            error =
                sink.overflow ? "response too large" : curl_easy_strerror(code);
            break;
        }
        long httpCode = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &httpCode);
        if (httpCode != 200) {
            error = "http " + std::to_string(httpCode);
            break;
        }
        if (sink.overflow) {
            error = "response too large";
            break;
        }
        body = std::move(sink.data);
        ok = true;
    } while (false);

    // Clear the body pointer before the buffer dies, so a later request can
    // never read freed memory even if it fails before setting it again.
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, nullptr);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, nullptr);
    curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, nullptr);
    curl_slist_free_all(headers);
    return ok;
}

int LlmWorker::progressCallback(void *userdata, curl_off_t, curl_off_t,
                                curl_off_t, curl_off_t) {
    const auto *transfer = static_cast<const TransferState *>(userdata);
    return transfer && transfer->worker->isCurrent(transfer->seq) ? 0 : 1;
}

size_t LlmWorker::writeCallback(char *ptr, size_t size, size_t nmemb,
                                void *userdata) {
    auto *sink = static_cast<WriteSink *>(userdata);
    const size_t realsize = size * nmemb;
    // Refuse to buffer more than the documented ceiling. Returning a short
    // count aborts the transfer, which surfaces as a transport error rather
    // than as a partial JSON document.
    if (sink->data.size() + realsize > MAX_RESPONSE_BYTES) {
        sink->overflow = true;
        return 0;
    }
    sink->data.append(ptr, realsize);
    return realsize;
}

} // namespace fcitx::chinesellm
