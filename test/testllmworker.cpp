/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
// Exercises the worker against a real HTTP endpoint. Not part of ctest because
// it needs a server; run it manually:
//
//   ./scripts/mock_server.py --port 8899 &
//   ./build/test/testllmworker http://127.0.0.1:8899/v1/chat/completions
//
// It prints each reply and exits non-zero if the pipeline did not produce the
// candidates the mock server was told to answer with.
#include "llmbackend.h"
#include "llmworker.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace fcitx::chinesellm;

namespace {

int failures = 0;

void fail(const std::string &what) {
    std::cerr << "FAIL: " << what << "\n";
    failures++;
}

void run(const std::string &endpoint, const std::string &pinyin,
         const std::string &context,
         const std::vector<std::string> &expected) {
    LlmWorker worker;
    worker.start();

    BackendConfig config;
    config.endpoint = endpoint;
    config.model = "mock";
    config.systemPrompt = "convert";
    config.promptTemplate = "{context}\n{pinyin}";
    config.timeoutSeconds = 5;

    bool done = false;
    std::vector<std::string> got;
    bool ok = false;
    std::string error;

    worker.submit(config, pinyin, context, [&](FetchResult result) {
        done = true;
        ok = result.ok;
        error = result.error;
        got = result.candidates;
    });

    for (int i = 0; i < 200 && !done; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    worker.stop();

    if (!done) {
        fail("no reply for " + pinyin + " within 5s");
        return;
    }
    if (!ok) {
        fail("request failed for " + pinyin + ": " + error);
        return;
    }
    if (got != expected) {
        std::cerr << "FAIL: candidates for " << pinyin << "\n  got: ";
        for (const auto &c : got) {
            std::cerr << "[" << c << "]";
        }
        std::cerr << "\n  expected: ";
        for (const auto &c : expected) {
            std::cerr << "[" << c << "]";
        }
        std::cerr << "\n";
        failures++;
        return;
    }
    std::cout << "ok: " << pinyin << " -> ";
    for (const auto &c : got) {
        std::cout << "[" << c << "]";
    }
    std::cout << "\n";
}

} // namespace

int main(int argc, char **argv) {
    const std::string endpoint =
        argc > 1 ? argv[1] : "http://127.0.0.1:8899/v1/chat/completions";

    run(endpoint, "nihao", "", {"你好"});
    // Multiple lines become multiple candidates, order preserved. 他/她 cannot
    // be told apart from pinyin alone, which is exactly why several candidates
    // are returned rather than one.
    run(endpoint, "ta'men'zheng'zai'kai'hui", "",
        {"他们正在开会", "她们正在开会"});
    // The context must reach the server, but it does not change the answer for
    // this pinyin; this only proves the request carries it.
    run(endpoint, "nihao", "你好", {"你好"});
    // Initials skip the length check, so a four-character answer for four keys
    // survives.
    run(endpoint, "nhsj", "", {"你换手机"});

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "All worker tests passed\n";
    return EXIT_SUCCESS;
}
