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

// All callback-owned values are published together through the future. Unlike
// polling a plain bool, this establishes a happens-before edge for the result.
std::optional<FetchResult> request(LlmWorker &worker, BackendConfig config,
                                   const std::string &pinyin = "nihao",
                                   const std::string &context = "",
                                   bool expectedStarted = true) {
    auto promise = std::make_shared<std::promise<FetchResult>>();
    auto future = promise->get_future();
    auto startedCalls = std::make_shared<std::atomic<int>>(0);
    auto startedOnWorker = std::make_shared<std::atomic<bool>>(false);
    auto repliedOnWorker = std::make_shared<std::atomic<bool>>(false);
    auto countedBeforeNotification = std::make_shared<std::atomic<bool>>(false);
    const auto callerThread = std::this_thread::get_id();
    const auto before = worker.requestsStarted();
    const auto seq = worker.submit(
        std::move(config), pinyin, context,
        [promise, repliedOnWorker, callerThread](FetchResult result) {
            *repliedOnWorker = std::this_thread::get_id() != callerThread;
            promise->set_value(std::move(result));
        },
        [&worker, startedCalls, startedOnWorker, countedBeforeNotification,
         callerThread, before] {
            ++*startedCalls;
            *startedOnWorker = std::this_thread::get_id() != callerThread;
            *countedBeforeNotification = worker.requestsStarted() == before + 1;
        });
    if (future.wait_for(6s) != std::future_status::ready) {
        check(false, "no reply for " + pinyin + " within 6s");
        return std::nullopt;
    }
    auto result = future.get();
    check(worker.requestsStarted() == before + (expectedStarted ? 1 : 0),
          "requestsStarted counts actual transfers, not submitted jobs");
    check(*startedCalls == (expectedStarted ? 1 : 0),
          "started callback runs exactly once per actual transfer");
    if (expectedStarted) {
        check(*startedOnWorker && *repliedOnWorker &&
                  *countedBeforeNotification,
              "started and reply callbacks run on worker thread; count is "
              "incremented before notification");
    }
    check(result.seq == seq && result.seq == worker.currentSeq(),
          "callback carries the current request sequence");
    check(result.pinyin == pinyin, "callback preserves request pinyin");
    return result;
}

void run(const BackendConfig &config, const std::string &pinyin,
         const std::string &context, const std::vector<std::string> &expected,
         bool expectedOk = true, const std::string &expectedError = "") {
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

void testQueuedCancellation(const std::string &endpoint) {
    const auto base = origin(endpoint);
    control(base, "/reset", true);
    LlmWorker worker;
    std::atomic<int> cancelledCallbacks{0};
    std::atomic<int> cancelledStarts{0};
    const auto seq = worker.submit(
        testConfig(endpoint), "ceshi", "",
        [&](FetchResult) { ++cancelledCallbacks; }, [&] { ++cancelledStarts; });
    check(worker.requestsStarted() == 0, "queued submission is not yet sent");
    worker.cancelAll();
    check(worker.requestsStarted() == 0, "queued cancellation is not counted");
    check(worker.currentSeq() != seq,
          "cancelAll invalidates a queued sequence");
    worker.start();
    const auto result = request(worker, testConfig(endpoint));
    worker.stop();
    check(result && result->ok &&
              result->candidates == std::vector<std::string>{"你好"},
          "worker accepts a new request after queued cancellation");
    check(cancelledCallbacks == 0 && cancelledStarts == 0,
          "queued cancellation suppresses reply and started callbacks");
    check(worker.requestsStarted() == 1,
          "only replacement transfer contributes to lifetime count");
    const auto stats = control(base, "/stats");
    check(stats.is_object() && stats.value("count", 0) == 1,
          "cancelled queued request is never sent");
}

void testInFlightCancellation(const std::string &endpoint, bool supersede) {
    const auto base = origin(endpoint);
    const std::string slowPath = "/slow/v1/chat/completions";
    control(base, "/reset", true);
    LlmWorker worker;
    worker.start();
    std::atomic<int> staleCallbacks{0};
    const auto oldSeq = worker.submit(testConfig(base + slowPath), "ceshi", "",
                                      [&](FetchResult) { ++staleCallbacks; });
    if (!waitForRequest(base, slowPath)) {
        worker.stop();
        return;
    }
    check(worker.requestsStarted() == 1,
          "in-flight request is counted before any result arrives");
    const auto started = std::chrono::steady_clock::now();
    if (!supersede) {
        worker.cancelAll();
        check(worker.currentSeq() != oldSeq,
              "cancelAll invalidates in-flight sequence");
    }
    const auto result = request(worker, testConfig(endpoint));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    worker.stop();
    check(result && result->ok &&
              result->candidates == std::vector<std::string>{"你好"},
          "new request succeeds after in-flight cancellation");
    check(staleCallbacks == 0,
          "superseded/cancelled transfer does not deliver stale callback");
    check(worker.requestsStarted() == 2,
          "cancelled in-flight transfer and replacement both count as sent");
    check(elapsed < 2500ms,
          "cancellation aborts slow transfer instead of waiting for its reply");
}

void testStopAndRestart(const std::string &endpoint) {
    const auto base = origin(endpoint);
    const std::string slowPath = "/slow/v1/chat/completions";
    control(base, "/reset", true);
    LlmWorker worker;
    worker.start();
    std::atomic<int> stoppedCallbacks{0};
    const auto seq = worker.submit(testConfig(base + slowPath), "ceshi", "",
                                   [&](FetchResult) { ++stoppedCallbacks; });
    if (!waitForRequest(base, slowPath)) {
        worker.stop();
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    worker.stop();
    check(std::chrono::steady_clock::now() - started < 2500ms,
          "stop aborts in-flight transfer without waiting for timeout");
    check(worker.currentSeq() != seq,
          "stop invalidates sequences already queued for delivery");
    check(stoppedCallbacks == 0, "stop suppresses in-flight callback");
    check(worker.requestsStarted() == 1,
          "stop retains the count of the aborted in-flight request");
    worker.start();
    check(worker.requestsStarted() == 1, "restart does not reset statistics");
    const auto result = request(worker, testConfig(endpoint));
    check(result && result->ok,
          "worker curl handle remains usable after restart");
    if (result) {
        worker.cancelAll();
        check(result->seq != worker.currentSeq(),
              "main-thread delivery can identify a result invalidated after "
              "callback");
    }
    worker.stop();
}

void testConcurrentSubmissions(const std::string &endpoint) {
    const auto base = origin(endpoint);
    control(base, "/reset", true);
    LlmWorker worker;
    auto promise = std::make_shared<std::promise<FetchResult>>();
    auto future = promise->get_future();
    constexpr size_t threads = 8;
    std::vector<uint64_t> sequences(threads);
    std::vector<std::thread> submitters;
    for (size_t i = 0; i < threads; ++i) {
        submitters.emplace_back([&, i] {
            sequences[i] =
                worker.submit(testConfig(endpoint), "nihao", "",
                              [promise](FetchResult result) {
                                  promise->set_value(std::move(result));
                              });
        });
    }
    for (auto &thread : submitters) {
        thread.join();
    }
    check(worker.requestsStarted() == 0,
          "concurrent submissions are not counted while queued");
    // Before start(), every concurrent submission is still queued. The job
    // retained under mutex_ must be exactly the one with the newest sequence.
    worker.start();
    if (future.wait_for(6s) == std::future_status::ready) {
        const auto result = future.get();
        check(result.ok && result.seq == worker.currentSeq(),
              "concurrent submissions retain the newest sequence");
        for (const auto seq : sequences) {
            check(seq <= result.seq,
                  "retained job is newer than every superseded job");
        }
    } else {
        check(false, "concurrent submissions lost the newest queued job");
    }
    worker.stop();
    const auto stats = control(base, "/stats");
    check(stats.is_object() && stats.value("count", 0) == 1 &&
              worker.requestsStarted() == 1,
          "concurrent queued submissions count only the one retained request");
}

void testInvalidUtf8Config(const std::string &endpoint) {
    const auto base = origin(endpoint);
    control(base, "/reset", true);
    LlmWorker worker;
    worker.start();
    auto invalidConfig = testConfig(endpoint);
    invalidConfig.systemPrompt += '\x80';
    const auto invalid =
        request(worker, std::move(invalidConfig), "nihao", "", false);
    check(worker.requestsStarted() == 0,
          "serialization failure does not count a network request");
    check(invalid && !invalid->ok && invalid->candidates.empty() &&
              invalid->error.starts_with("request processing failed:"),
          "invalid UTF-8 configuration reports failure without terminating "
          "worker");
    const auto valid = request(worker, testConfig(endpoint));
    worker.stop();
    check(valid && valid->ok,
          "worker recovers after JSON serialization exception");
    const auto stats = control(base, "/stats");
    check(stats.is_object() && stats.value("count", 0) == 1,
          "invalid UTF-8 configuration is rejected before network access");
}

void testTransportFailure(const std::string &endpoint) {
    const auto base = origin(endpoint);
    control(base, "/reset", true);
    const auto health = control(base, "/healthz");
    if (!health.is_object() || !health.contains("dead_port")) {
        check(false, "mock health has no reserved dead port");
        return;
    }
    auto config = testConfig("http://127.0.0.1:" +
                             std::to_string(health.at("dead_port").get<int>()) +
                             "/v1/chat/completions");
    LlmWorker worker;
    worker.start();
    const auto result = request(worker, config);
    worker.stop();
    check(result && !result->ok && result->candidates.empty() &&
              worker.requestsStarted() == 1,
          "connection refusal counts a started request even without a reply");
    const auto stats = control(base, "/stats");
    check(stats.is_object() && stats.value("count", 0) == 0,
          "transport failure is counted independently of server POSTs");
}

void testHandleReuse(const std::string &endpoint) {
    const auto base = origin(endpoint);
    LlmWorker worker;
    worker.start();
    const auto failed =
        request(worker, testConfig(base + "/error/v1/chat/completions"));
    const auto valid = request(worker, testConfig(endpoint));
    worker.stop();
    check(failed && !failed->ok, "reused handle reports first HTTP failure");
    check(valid && valid->ok &&
              valid->candidates == std::vector<std::string>{"你好"},
          "reused handle succeeds after HTTP failure");
}

} // namespace

int main(int argc, char **argv) {
    const std::string endpoint =
        argc > 1 ? argv[1] : "http://127.0.0.1:8899/v1/chat/completions";
    const auto config = testConfig(endpoint);
    run(config, "nihao", "", {"你好"});
    run(config, "ta'men'zheng'zai'kai'hui", "",
        {"他们正在开会", "她们正在开会"});
    run(config, "nihao", "你好", {"你好"});
    run(config, "nhsj", "", {"你换手机"});

    const auto base = origin(endpoint);
    run(testConfig(base + "/error/v1/chat/completions"), "nihao", "", {}, false,
        "http 500");
    run(testConfig(base + "/invalid/v1/chat/completions"), "nihao", "", {},
        false, "invalid completion response");
    run(testConfig(base + "/truncated/v1/chat/completions"), "nihao", "", {},
        false, "invalid completion response");
    run(testConfig(base + "/oversized/v1/chat/completions"), "nihao", "", {},
        false, "response too large");
    run(testConfig(base + "/empty/v1/chat/completions"), "nihao", "", {});
    auto timeoutConfig = testConfig(base + "/slow/v1/chat/completions");
    timeoutConfig.timeoutSeconds = 1;
    run(timeoutConfig, "nihao", "", {}, false, "Timeout");
    testRedirects(endpoint);
    testQueuedCancellation(endpoint);
    testInFlightCancellation(endpoint, false);
    testInFlightCancellation(endpoint, true);
    testStopAndRestart(endpoint);
    testConcurrentSubmissions(endpoint);
    testInvalidUtf8Config(endpoint);
    testTransportFailure(endpoint);
    testHandleReuse(endpoint);

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "All worker tests passed\n";
    return EXIT_SUCCESS;
}
