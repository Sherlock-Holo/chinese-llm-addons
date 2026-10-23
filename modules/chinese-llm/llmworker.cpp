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

// The log category is defined in chinese-llm.cpp; declaring it here avoids a
// duplicate definition while keeping the same category name in the log.
FCITX_DECLARE_LOG_CATEGORY(chinesellm);

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
        (curl_easy_setopt(curl_, CURLOPT_UNRESTRICTED_AUTH, 0L) == CURLE_OK);
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
        pending_.reset();
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

uint64_t LlmWorker::submit(BackendConfig config, std::string pinyin,
                           std::string context, FetchCallback callback) {
    const uint64_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
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
    // Bumping the sequence is enough to make every in-flight reply stale; the
    // request itself will run to completion and be discarded.
    seq_.fetch_add(1, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
    }
}

bool LlmWorker::isCurrent(uint64_t seq) const {
    return seq == seq_.load(std::memory_order_relaxed);
}

void LlmWorker::run() {
    while (true) {
        std::optional<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_.load() || pending_.has_value(); });
            if (stop_.load()) {
                return;
            }
            job = std::move(pending_);
            pending_.reset();
        }
        if (!job) {
            continue;
        }

        std::string body;
        std::string error;
        const bool ok = perform(*job, body, error);
        if (!isCurrent(job->seq)) {
            FCITX_DEBUG() << "chinese-llm: discarding superseded reply for "
                          << job->pinyin;
            continue;
        }

        FetchResult result;
        result.ok = ok;
        result.pinyin = job->pinyin;
        result.error = std::move(error);
        if (ok) {
            result.candidates =
                sanitize(parseResponse(body), job->pinyin, 0);
        }
        if (job->callback) {
            job->callback(std::move(result));
        }
    }
}

bool LlmWorker::perform(const Job &job, std::string &body, std::string &error) {
    const auto spec = buildRequest(job.config, job.pinyin, job.context);

    curl_slist *headers = nullptr;
    for (const auto &header : spec.headers) {
        headers = curl_slist_append(headers, header.c_str());
    }

    WriteSink sink;
    // The timeout must be per request: a shorter budget is deliberately used
    // here than cloudpinyin's hardcoded 10s because a slow reply is useless
    // once the user has typed past it.
    const long timeout =
        std::clamp<long>(job.config.timeoutSeconds > 0
                             ? job.config.timeoutSeconds
                             : 5,
                         1, 60);
    const long connectTimeout = std::min<long>(timeout, 3);

    bool ok = false;
    do {
        if (curl_easy_setopt(curl_, CURLOPT_URL, spec.url.c_str()) != CURLE_OK) {
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
        if (curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &sink) != CURLE_OK) {
            error = "invalid write sink";
            break;
        }

        const auto code = curl_easy_perform(curl_);
        if (code != CURLE_OK) {
            error = curl_easy_strerror(code);
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
    curl_slist_free_all(headers);
    return ok;
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
