/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
// Exercises the worker against scripts/mock_server.py, including its redirect,
// error and delayed-response endpoints. Driven by scripts/smoke.sh, not ctest.
#include "llmbackend.h"
#include "llmworker.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace fcitx::chinesellm;
using namespace std::chrono_literals;

namespace {

int failures = 0;

void check(bool condition, const std::string &what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

BackendConfig testConfig(const std::string &endpoint) {
    BackendConfig config;
    config.endpoint = endpoint;
    config.model = "mock";
    config.systemPrompt = "convert";
    config.promptTemplate = "{context}\n{pinyin}";
    config.timeoutSeconds = 5;
    return config;
}

std::string origin(const std::string &endpoint) {
    const auto scheme = endpoint.find("://");
    const auto path =
        endpoint.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    return endpoint.substr(0, path);
}

// Collects every event of one job. The result is published through the future,
// which establishes a happens-before edge for it.
struct Collector {
    std::promise<void> done;
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    FetchResult result;

    FetchHandler handler() {
        return [this](FetchStage stage, const FetchResult &value) {
            if (stage == FetchStage::Started) {
                ++started;
                return;
            }
            result = value;
            ++finished;
            done.set_value();
        };
    }
};

// Submits one request and waits for its Finished event.
std::optional<FetchResult> request(LlmWorker &worker, BackendConfig config,
                                   const std::string &pinyin = "nihao",
                                   const std::string &context = "",
                                   bool expectedStarted = true) {
    Collector collector;
    auto future = collector.done.get_future();
    const auto before = worker.requestsStarted();
    const auto seq = worker.submit(std::move(config), pinyin, context,
                                   collector.handler());
    if (future.wait_for(6s) != std::future_status::ready) {
        check(false, "no reply for " + pinyin + " within 6s");
        return std::nullopt;
    }
    auto result = collector.result;
    check(collector.finished == 1,
          "every job is finished exactly once");
    check(worker.requestsStarted() == before + (expectedStarted ? 1 : 0),
          "requestsStarted counts actual transfers, not submitted jobs");
    check(collector.started == (expectedStarted ? 1 : 0),
          "Started is reported exactly once per actual transfer");
    check(result.seq == seq && result.seq == worker.currentSeq() &&
              result.pinyin == pinyin && !result.cancelled &&
              result.started == expectedStarted,
          "result carries the request identity and is current");
    return result;
}

void run(const BackendConfig &config, const std::string &pinyin,
         const std::string &context, const std::vector<std::string> &expected,
         bool expectedOk = true, const std::string &expectedError = "",
         long expectedHttpStatus = 200) {
    LlmWorker worker;
    check(worker.requestsStarted() == 0,
          "new worker starts with zero requests");
    worker.start();
    check(worker.requestsStarted() == 0,
          "start alone does not count a request");
    const auto result = request(worker, config, pinyin, context);
    worker.stop();
    check(worker.requestsStarted() == 1,
          "HTTP errors, empty replies and redirects each count one logical "
          "request; stop does not reset it");
    if (!result) {
        return;
    }
    check(result->ok == expectedOk,
          config.endpoint + " response status: " + result->error);
    check(result->candidates == expected,
          config.endpoint + " candidates for " + pinyin);
    check(result->httpStatus == expectedHttpStatus,
          "HTTP metadata is captured for success and failure");
    if (expectedError == "Timeout") {
        check(result->elapsedMs >= 900 && result->elapsedMs < 2500,
              "timeout duration includes network wait, not just parsing");
    }
    if (!expectedError.empty()) {
        check(result->error.find(expectedError) != std::string::npos,
              config.endpoint + " error: " + result->error);
    }
}

size_t collectBody(char *ptr, size_t size, size_t nmemb, void *userdata) {
    const auto bytes = size * nmemb;
    static_cast<std::string *>(userdata)->append(ptr, bytes);
    return bytes;
}

// The mock records a request before delaying its reply. Polling these records
// lets cancellation tests wait for an actual in-flight transfer, not a sleep
// that can accidentally test cancellation before the job starts.
nlohmann::json control(const std::string &base, const std::string &path,
                       bool post = false) {
    auto *curl = curl_easy_init();
    if (!curl) {
        check(false, "cannot initialize mock control client");
        return {};
    }
    std::string body;
    const auto url = base + path;
    const bool configured =
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L) == CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collectBody) ==
            CURLE_OK &&
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body) == CURLE_OK &&
        (!post || (curl_easy_setopt(curl, CURLOPT_POST, 1L) == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "") == CURLE_OK));
    const auto code = configured ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status != 200) {
        check(false, "mock control request failed: " + path);
        return {};
    }
    auto parsed = nlohmann::json::parse(body, nullptr, false);
    check(!parsed.is_discarded(), "mock control returns JSON: " + path);
    return parsed.is_discarded() ? nlohmann::json{} : parsed;
}

bool waitForRequest(const std::string &base, const std::string &path) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        const auto stats = control(base, "/stats");
        if (stats.is_object() && stats.contains("requests")) {
            for (const auto &record : stats.at("requests")) {
                if (record.value("path", "") == path) {
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    check(false, "mock did not observe in-flight request " + path);
    return false;
}

void testRedirects(const std::string &endpoint) {
    const auto base = origin(endpoint);
    for (const auto code : {301, 302, 303, 307, 308}) {
        for (const bool crossHost : {false, true}) {
            control(base, "/reset", true);
            const auto path = std::string(crossHost ? "/redirect-cross-host/"
                                                    : "/redirect/") +
                              std::to_string(code);
            auto config = testConfig(base + path);
            config.apiKey = "redirect-secret";
            run(config, "nihao", "redirect context", {"你好"});
            const auto stats = control(base, "/stats");
            if (!stats.is_object() || !stats.contains("requests")) {
                check(false, "redirect stats missing requests");
                continue;
            }
            const auto &records = stats.at("requests");
            check(records.size() == 2,
                  "redirect makes exactly two POST requests");
            if (records.size() != 2) {
                continue;
            }
            check(records.at(0).value("method", "") == "POST" &&
                      records.at(1).value("method", "") == "POST",
                  "redirect " + std::to_string(code) + " preserves POST");
            check(records.at(0).at("body") == records.at(1).at("body") &&
                      records.at(1).value("pinyin", "") == "nihao" &&
                      records.at(1).value("user", "") ==
                          "redirect context\nnihao",
                  "redirect preserves JSON body and context");
            check(records.at(0).value("authorization", "") ==
                      "Bearer redirect-secret",
                  "redirect origin receives configured Authorization");
            check(records.at(1).value("authorization", "") ==
                      (crossHost ? "" : "Bearer redirect-secret"),
                  crossHost ? "cross-host redirect does not leak Authorization"
                            : "same-host redirect preserves Authorization");
        }
    }
}

// The main reason the worker exists: a newer request must replace an older
// one immediately, whether it is still queued or already on the wire.
void testSupersede(const std::string &endpoint) {
    const auto base = origin(endpoint);
    const std::string slowPath = "/slow/v1/chat/completions";
    control(base, "/reset", true);
    LlmWorker worker;
    worker.start();

    Collector old;
    auto oldDone = old.done.get_future();
    const auto oldSeq = worker.submit(testConfig(base + slowPath), "ceshi", "",
                                      old.handler());
    if (!waitForRequest(base, slowPath)) {
        worker.stop();
        return;
    }
    const auto submitted = std::chrono::steady_clock::now();
    Collector fresh;
    auto freshDone = fresh.done.get_future();
    const auto freshSeq =
        worker.submit(testConfig(endpoint), "nihao", "", fresh.handler());
    check(freshDone.wait_for(6s) == std::future_status::ready,
          "replacement request completes");
    const auto latency = std::chrono::steady_clock::now() - submitted;
    check(oldDone.wait_for(0s) == std::future_status::ready,
          "the superseded transfer was reported");
    worker.stop();

    check(fresh.result.ok && fresh.result.seq == freshSeq &&
              fresh.result.candidates == std::vector<std::string>{"你好"},
          "new request succeeds after superseding an in-flight one");
    check(old.finished == 1 && old.result.seq == oldSeq &&
              old.result.cancelled && !old.result.ok && old.result.started &&
              old.result.error == "cancelled",
          "superseded in-flight job is finished once as cancelled");
    check(worker.requestsStarted() == 2,
          "cancelled transfer and its replacement both count as started");
    // Without a wakeup the abort is only noticed on curl's progress tick,
    // about once per second, which stalls typing on a slow model.
    check(latency < 500ms,
          "superseding aborts the old transfer immediately, not on the next "
          "progress tick");
}

// A queued job that is replaced or cancelled never reaches curl, but its owner
// must still hear about it so per-request bookkeeping cannot leak.
void testQueuedJobsAreReported(const std::string &endpoint) {
    const auto base = origin(endpoint);
    control(base, "/reset", true);
    LlmWorker worker;
    Collector first;
    Collector second;
    const auto firstSeq =
        worker.submit(testConfig(endpoint), "ceshi", "", first.handler());
    const auto secondSeq =
        worker.submit(testConfig(endpoint), "nihao", "", second.handler());
    check(first.finished == 1 && first.started == 0 &&
              first.result.seq == firstSeq && first.result.cancelled &&
              !first.result.started,
          "a replaced queued job is reported as cancelled without starting");
    worker.cancelAll();
    check(second.finished == 1 && second.result.seq == secondSeq &&
              second.result.cancelled,
          "cancelAll reports the queued job");
    check(worker.requestsStarted() == 0, "queued jobs are never counted");
    worker.start();
    const auto result = request(worker, testConfig(endpoint));
    worker.stop();
    check(result && result->ok, "worker is usable after cancellation");
    const auto stats = control(base, "/stats");
    check(stats.is_object() && stats.value("count", 0) == 1,
          "cancelled queued requests are never sent");
}

// Chatty and unsafe replies must reach the main thread as diagnostics, never
// as candidates.
void testDiagnostics(const std::string &endpoint) {
    const auto base = origin(endpoint);
    LlmWorker worker;
    worker.start();
    const auto filtered =
        request(worker, testConfig(base + "/all-filtered/v1/chat/completions"),
                "ta'men'zheng'zai'kai'hui");
    check(filtered && filtered->ok && filtered->candidates.empty() &&
              !filtered->content.empty() && !filtered->rejected.empty(),
          "a fully rejected reply keeps raw content and rejection reasons");
    const auto truncated =
        request(worker, testConfig(base + "/truncated/v1/chat/completions"));
    check(truncated && !truncated->ok && truncated->candidates.empty() &&
              truncated->rejected.size() == 1 &&
              truncated->rejected[0].reason == "incomplete_response",
          "a token-limited reply is diagnosed but never delivered");
    const auto http500 =
        request(worker, testConfig(base + "/error/v1/chat/completions"));
    check(http500 && !http500->ok && http500->httpStatus == 500 &&
              http500->content.empty(),
          "HTTP errors keep the status but not the server's error body");
    worker.stop();
}

} // namespace

int main(int argc, char **argv) {
    const std::string endpoint =
        argc > 1 ? argv[1] : "http://127.0.0.1:8899/v1/chat/completions";
    const auto base = origin(endpoint);
    const auto config = testConfig(endpoint);
    run(config, "nihao", "", {"你好"});
    run(config, "ta'men'zheng'zai'kai'hui", "",
        {"他们正在开会", "她们正在开会"});
    run(config, "bang'wo'push'yi'xia", "", {"帮我push一下"});
    run(testConfig(base + "/error/v1/chat/completions"), "nihao", "", {}, false,
        "http 500", 500);
    run(testConfig(base + "/oversized/v1/chat/completions"), "nihao", "", {},
        false, "response too large");
    auto timeoutConfig = testConfig(base + "/slow/v1/chat/completions");
    timeoutConfig.timeoutSeconds = 1;
    run(timeoutConfig, "nihao", "", {}, false, "Timeout", 0);
    testRedirects(endpoint);
    testSupersede(endpoint);
    testQueuedJobsAreReported(endpoint);
    testDiagnostics(endpoint);

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "All worker tests passed\n";
    return EXIT_SUCCESS;
}
