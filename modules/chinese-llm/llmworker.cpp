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
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace fcitx::chinesellm {

namespace {

struct WriteSink {
    std::string data;
    bool overflow = false;
};

// Diagnostic extraction must not loosen parseResponse's completion contract.
// Only preserve a string message.content from a valid JSON completion shape,
// never server error envelopes, request data, or malformed UTF-8 (JSON rejects
// it). Explicit non-stop finish reasons remain failures for candidate delivery.
bool extractDiagnosticContent(std::string_view body, std::string &content,
                              bool &incomplete) {
    content.clear();
    incomplete = false;
    const auto first = body.find('{');
    const auto last = body.rfind('}');
    if (first == std::string_view::npos || last == std::string_view::npos ||
        last < first) {
        return false;
    }
    try {
        const auto reply =
            nlohmann::json::parse(body.substr(first, last - first + 1));
        const auto &choices = reply.at("choices");
        if (!choices.is_array() || choices.empty()) {
            return false;
        }
        const auto &choice = choices.at(0);
        const auto &value = choice.at("message").at("content");
        if (!value.is_string()) {
            return false;
        }
        content = value.get<std::string>();
        const auto finish = choice.find("finish_reason");
        incomplete = finish != choice.end() && finish->is_string() &&
                     finish->get<std::string>() != "stop";
        return true;
    } catch (const std::exception &) {
        content.clear();
        return false;
    }
}

// The JSON decoder has already validated these bytes. Do not split the final
// scalar in a rejected incomplete-response snippet; history escapes controls.
std::string boundedDiagnosticContent(std::string_view content) {
    size_t end = std::min<size_t>(content.size(), 1024);
    while (end > 0 && end < content.size() &&
           (static_cast<unsigned char>(content[end]) & 0xC0) == 0x80) {
        --end;
    }
    return std::string(content.substr(0, end));
}

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
                           std::string context, FetchCallback callback,
                           std::function<void()> started,
                           FetchCallback observed) {
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
                             .callback = std::move(callback),
                             .started = std::move(started),
                             .observed = std::move(observed)});
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
        std::optional<std::chrono::steady_clock::time_point> startedAt;
        try {
            std::string body;
            result.ok = perform(*job, body, result, startedAt);
            // A completed body remains useful diagnostics after supersede, or
            // even with a non-200 status. Never expose a partial/invalid reply
            // as candidates through the current-only callback.
            const bool complete = parseResponse(body, result.content);
            bool incomplete = false;
            if (!complete) {
                extractDiagnosticContent(body, result.content, incomplete);
            }
            if (complete) {
                auto sanitized =
                    sanitizeDetailed(result.content, result.pinyin, 0);
                // Preserve a transport rejection (e.g. byte cap) even if the
                // buffered prefix happens to contain a parseable completion.
                for (auto &rejected : sanitized.rejected) {
                    if (result.rejected.size() == 32) {
                        break;
                    }
                    result.rejected.push_back(std::move(rejected));
                }
                if (result.ok || !isCurrent(job->seq)) {
                    result.candidates = std::move(sanitized.candidates);
                }
            } else {
                if (incomplete) {
                    result.rejected.push_back(
                        {boundedDiagnosticContent(result.content),
                         "incomplete_response"});
                }
                if (result.ok) {
                    result.ok = false;
                    result.error = incomplete ? "incomplete completion response"
                                              : "invalid completion response";
                }
            }
        } catch (const std::exception &) {
            // Serialization exceptions can quote configuration, context or
            // malformed bytes. Use a fixed category, never e.what() or logs.
            result.ok = false;
            result.candidates.clear();
            result.content.clear();
            result.rejected.clear();
            result.error = "request processing failed";
        }
        if (result.started && startedAt) {
            result.elapsedMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - *startedAt)
                    .count());
        }
        result.cancelled = !isCurrent(job->seq);
        if (result.cancelled && !result.ok) {
            result.error = "cancelled";
        }
        // Observation is intentionally not current-only and receives a copy.
        // It also runs while stop() waits; the owner must guard posted UI work.
        if (job->observed) {
            job->observed(result);
        }
        // Parsing/sanitizing/observation may take time. Check immediately before
        // delivery; the main-thread consumer must also validate result.seq,
        // since a request can be superseded after this check or while queued.
        if (job->callback && isCurrent(job->seq)) {
            job->callback(std::move(result));
        }
    }
}

bool LlmWorker::perform(
    const Job &job, std::string &body, FetchResult &result,
    std::optional<std::chrono::steady_clock::time_point> &startedAt) {
    auto &error = result.error;
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

        // pending_ can be replaced without ever reaching curl. Count only
        // logical transfer attempts here, not submissions or redirect hops.
        // A started transfer remains counted even when its reply is discarded.
        startedAt = std::chrono::steady_clock::now();
        requestsStarted_.fetch_add(1, std::memory_order_relaxed);
        if (job.started) {
            job.started();
        }
        result.started = true;
        const auto code = curl_easy_perform(curl_);
        // CURLINFO_RESPONSE_CODE can be available even when a response body
        // overflows, a transfer times out, or a late cancellation aborts it.
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &result.httpStatus);
        body = std::move(sink.data);
        if (sink.overflow) {
            error = "response too large";
            // The rejected chunk was intentionally not buffered. Record the
            // category without retaining a partial response-envelope snippet.
            result.rejected.push_back({"", "response_too_large"});
            break;
        }
        if (code != CURLE_OK) {
            error = curl_easy_strerror(code); // fixed libcurl message, no input
            break;
        }
        if (result.httpStatus != 200) {
            error = "http " + std::to_string(result.httpStatus);
            break;
        }
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
