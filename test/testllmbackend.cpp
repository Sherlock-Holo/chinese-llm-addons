/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "llmbackend.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace fcitx::chinesellm;

namespace {

int failures = 0;

void check(bool cond, const std::string &what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        failures++;
    }
}

template <typename T>
void checkEq(const T &actual, const T &expected, const std::string &what) {
    if (!(actual == expected)) {
        std::cerr << "FAIL: " << what << "\n  actual:   " << actual
                  << "\n  expected: " << expected << "\n";
        failures++;
    }
}

void checkCandidates(const std::vector<std::string> &actual,
                     const std::vector<std::string> &expected,
                     const std::string &what) {
    if (actual == expected) {
        return;
    }
    std::cerr << "FAIL: " << what << "\n  actual: ";
    for (const auto &c : actual) {
        std::cerr << "[" << c << "]";
    }
    std::cerr << "\n  expected: ";
    for (const auto &c : expected) {
        std::cerr << "[" << c << "]";
    }
    std::cerr << "\n";
    failures++;
}

BackendConfig testConfig() {
    BackendConfig config;
    config.endpoint = "http://127.0.0.1:11434/v1/chat/completions";
    config.model = "qwen3:0.6b";
    config.systemPrompt = "Convert pinyin to Han characters.";
    config.promptTemplate = "上下文：{context}\n按键：{pinyin}\n结果：";
    config.maxTokens = 32;
    return config;
}

void testRenderPrompt() {
    checkEq(renderPrompt("上下文：{context}\n按键：{pinyin}\n结果：", "nihao",
                         "你好"),
            std::string("上下文：你好\n按键：nihao\n结果："),
            "renderPrompt substitutes both placeholders");
    checkEq(renderPrompt("{pinyin}{pinyin}", "ab", "x"), std::string("abab"),
            "renderPrompt substitutes repeated placeholders");
    checkEq(renderPrompt("no placeholder", "ab", "x"),
            std::string("no placeholder"),
            "renderPrompt leaves text without placeholders alone");
    checkEq(renderPrompt("{context}", "ab", ""), std::string(""),
            "renderPrompt substitutes empty context");
    // A stray brace must not be mistaken for a placeholder.
    checkEq(renderPrompt("{pinyi}", "ab", ""), std::string("{pinyi}"),
            "renderPrompt ignores unknown placeholders");
}

void testBuildRequest() {
    auto config = testConfig();
    const auto spec = buildRequest(config, "nihao", "你好");

    checkEq(spec.url, config.endpoint, "buildRequest keeps the endpoint");
    check(spec.body.find("\"model\":\"qwen3:0.6b\"") != std::string::npos,
          "buildRequest embeds the model");
    check(spec.body.find("\"stream\":false") != std::string::npos,
          "buildRequest disables streaming");
    check(spec.body.find("\"temperature\":0.0") != std::string::npos,
          "buildRequest sets temperature");
    check(spec.body.find("\"max_tokens\":32") != std::string::npos,
          "buildRequest sets max_tokens");
    check(spec.body.find("nihao") != std::string::npos,
          "buildRequest embeds the pinyin");
    check(spec.body.find("Convert pinyin to Han characters.") !=
              std::string::npos,
          "buildRequest embeds the system prompt");
    // The body must be valid JSON, which dump() guarantees; a raw check that it
    // starts and ends with braces catches accidental string concatenation.
    check(!spec.body.empty() && spec.body.front() == '{' &&
              spec.body.back() == '}',
          "buildRequest produces a JSON object");

    bool hasContentType = false;
    bool hasAuth = false;
    for (const auto &header : spec.headers) {
        if (header == "Content-Type: application/json") {
            hasContentType = true;
        }
        if (header.starts_with("Authorization:")) {
            hasAuth = true;
        }
    }
    check(hasContentType, "buildRequest sets Content-Type");
    // An empty key means an unauthenticated local server; sending a bare
    // "Bearer " makes llama.cpp and ollama reject the request.
    check(!hasAuth, "buildRequest omits Authorization when the key is empty");

    config.apiKey = "secret";
    const auto withKey = buildRequest(config, "nihao", "");
    bool foundKey = false;
    for (const auto &header : withKey.headers) {
        if (header == "Authorization: Bearer secret") {
            foundKey = true;
        }
    }
    check(foundKey, "buildRequest sends Authorization when a key is set");
}

void testParseResponse() {
    const std::string ok = R"({"choices":[{"message":{"content":"你好"}}]})";
    checkEq(parseResponse(ok), std::string("你好"),
            "parseResponse reads content");

    // A fenced block with surrounding whitespace still parses.
    const std::string fenced = "  "
                               "\n```json\n{\"choices\":[{\"message\":{"
                               "\"content\":\"你好\"}}]}\n```\n ";
    checkEq(parseResponse(fenced), std::string("你好"),
            "parseResponse tolerates a fenced block");

    // Usage statistics and other keys must not confuse the parser.
    const std::string full =
        R"({"id":"x","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"你好"},"finish_reason":"stop"}],"usage":{"total_tokens":12}})";
    checkEq(parseResponse(full), std::string("你好"),
            "parseResponse ignores extra fields");

    checkEq(parseResponse(""), std::string(""), "parseResponse rejects empty");
    checkEq(parseResponse("not json"), std::string(""),
            "parseResponse rejects non-JSON");
    checkEq(parseResponse(R"({"choices":[]})"), std::string(""),
            "parseResponse rejects an empty choices array");
    checkEq(parseResponse(R"({"choices":[{"message":{}}]})"), std::string(""),
            "parseResponse rejects missing content");
    checkEq(parseResponse(R"({"choices":[{"message":{"content":null}}]})"),
            std::string(""), "parseResponse rejects null content");
    checkEq(parseResponse(R"({"choices":[{"message":{"content":42}}]})"),
            std::string(""), "parseResponse rejects non-string content");
    checkEq(parseResponse(R"({"error":{"message":"boom"}})"), std::string(""),
            "parseResponse rejects an error reply");
    checkEq(
        parseResponse(
            R"({"choices":[{"message":{"content":"你好"},"finish_reason":"length"}]})"),
        std::string(""), "parseResponse rejects token-limited Han text");
    checkEq(
        parseResponse(
            R"({"choices":[{"message":{"content":"你好"},"finish_reason":"content_filter"}]})"),
        std::string(""), "parseResponse rejects content-filtered text");
    checkEq(
        parseResponse(
            R"({"choices":[{"message":{"content":"你好"},"finish_reason":42}]})"),
        std::string(""), "parseResponse rejects malformed finish_reason");
    checkEq(
        parseResponse(
            R"({"choices":[{"message":{"content":"你好"},"finish_reason":null}]})"),
        std::string("你好"), "parseResponse tolerates null finish_reason");

    std::string content = "stale";
    check(
        parseResponse(
            R"({"choices":[{"message":{"content":""},"finish_reason":"stop"}]})",
            content) &&
            content.empty(),
        "parseResponse distinguishes valid empty content from failure");
    content = "stale";
    check(!parseResponse("not json", content) && content.empty(),
          "parseResponse clears content on parse failure");
}

void testCountSyllables() {
    checkEq(countSyllables("ni'hao"), size_t{2}, "countSyllables apostrophe");
    checkEq(countSyllables("ni hao"), size_t{2}, "countSyllables space");
    checkEq(countSyllables("wo'zai'xue'xi"), size_t{4},
            "countSyllables four syllables");
    // No separator: the boundary is unknown, so no check is possible.
    checkEq(countSyllables("nihao"), size_t{0},
            "countSyllables returns 0 without separators");
    checkEq(countSyllables("nhsj"), size_t{0},
            "countSyllables returns 0 for initials");
    checkEq(countSyllables(""), size_t{0}, "countSyllables empty");
    // Repeated separators must not inflate the count.
    checkEq(countSyllables("ni''hao"), size_t{2},
            "countSyllables ignores empty segments");
}

void testIsAllHan() {
    check(isAllHan("你好"), "isAllHan accepts Han characters");
    check(isAllHan("你好世界"), "isAllHan accepts a longer string");
    check(isAllHan("阿·勒泰"), "isAllHan accepts an interpunct");
    check(isAllHan("你好，世界"), "isAllHan accepts full-width comma");
    check(!isAllHan(""), "isAllHan rejects empty");
    check(!isAllHan("hello"), "isAllHan rejects ASCII");
    check(!isAllHan("你好abc"), "isAllHan rejects mixed ASCII");
    check(!isAllHan("你好123"), "isAllHan rejects digits");
    check(!isAllHan("，"), "isAllHan rejects punctuation only");
    check(isAllHan("𠀀你好"), "isAllHan accepts supplementary-plane Han");
    check(!isAllHan(std::string("你好") + '\x80'),
          "isAllHan rejects a stray continuation byte");
    check(!isAllHan(std::string("你好") + '\xe3'),
          "isAllHan rejects a truncated UTF-8 code point");
}

void testIsPlainPinyin() {
    check(isPlainPinyin("nihao"), "isPlainPinyin accepts full spelling");
    check(isPlainPinyin("ni'hao"), "isPlainPinyin accepts apostrophes");
    check(isPlainPinyin("ni hao"), "isPlainPinyin accepts spaces");
    check(isPlainPinyin("nhsj"), "isPlainPinyin accepts initials");
    check(!isPlainPinyin(""), "isPlainPinyin rejects empty");
    check(!isPlainPinyin("你好"), "isPlainPinyin rejects Han characters");
    // This is the shape the engine produces once the user starts selecting,
    // which is exactly where we must stay out of the way.
    check(!isPlainPinyin("你好 ni hao"),
          "isPlainPinyin rejects a partially selected preedit");
    check(!isPlainPinyin("ni3hao"), "isPlainPinyin rejects digits");
    check(!isPlainPinyin("   "), "isPlainPinyin rejects whitespace only");
}

void testSanitize() {
    // The common case.
    checkCandidates(sanitize("你好", "ni'hao", 3), {"你好"},
                    "sanitize accepts a matching single candidate");

    // The model tends to politely add a full stop; a bare key never wants one.
    checkCandidates(sanitize("你好。", "ni'hao", 3), {"你好"},
                    "sanitize strips trailing full-width full stop");
    checkCandidates(sanitize("\"你好\"", "ni'hao", 3), {"你好"},
                    "sanitize strips quotes");

    // Several lines become several candidates, in the model's order.
    checkCandidates(sanitize("我们去吃饭吧\n我们去吃饭不\n我们去吃饭呗",
                             "wo'men'qu'chi'fan'ba", 3),
                    {"我们去吃饭吧", "我们去吃饭不", "我们去吃饭呗"},
                    "sanitize splits multiple lines into candidates");
    checkCandidates(sanitize("我们去吃饭吧\n我们去吃饭不\n我们去吃饭呗",
                             "wo'men'qu'chi'fan'ba", 2),
                    {"我们去吃饭吧", "我们去吃饭不"},
                    "sanitize honours maxCandidates");

    // Duplicates are a UI bug in waiting.
    checkCandidates(sanitize("你好\n你好", "ni'hao", 3), {"你好"},
                    "sanitize deduplicates");
    checkCandidates(sanitize("你好\n你好\n拟好", "ni'hao", 2), {"你好", "拟好"},
                    "sanitize counts unique candidates toward the limit");
    checkCandidates(sanitize("阿·勒泰", "a'le'tai", 3), {"阿·勒泰"},
                    "sanitize counts Han rather than internal punctuation");
    checkCandidates(sanitize("你·好", "ni'hao'ma", 3), {},
                    "sanitize does not count punctuation as a missing Han");
    checkCandidates(sanitize("𠀀好", "ni'hao", 3), {"𠀀好"},
                    "sanitize counts supplementary-plane Han as one syllable");
    checkCandidates(sanitize(std::string("你好") + '\x80', "ni'hao", 3), {},
                    "sanitize does not strip invalid UTF-8 into valid text");
    checkCandidates(sanitize("你好。 ，！? \t", "ni'hao", 3), {"你好"},
                    "sanitize strips mixed trailing punctuation and spaces");

    std::string noisy = "你好";
    while (noisy.size() + std::string_view("。 ").size() <=
           MAX_RESPONSE_BYTES) {
        noisy += "。 ";
    }
    const auto started = std::chrono::steady_clock::now();
    checkCandidates(sanitize(noisy, "ni'hao", 3), {"你好"},
                    "sanitize handles a response-sized punctuation suffix");
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
          "sanitize strips long punctuation in linear time");

    // A length mismatch means the model invented or dropped a character; the
    // commit path is unconditional, so this must never be shown.
    checkCandidates(sanitize("你好吗", "ni'hao", 3), {},
                    "sanitize drops a too-long candidate");
    checkCandidates(sanitize("你", "ni'hao", 3), {},
                    "sanitize drops a too-short candidate");

    // Without separators the syllable count is unknown, so only the Han check
    // applies and the candidate survives.
    checkCandidates(sanitize("你换手机", "nhsj", 3), {"你换手机"},
                    "sanitize skips the length check for initials");

    // Anything non-Han is rejected outright rather than committed.
    checkCandidates(sanitize("hello", "ni'hao", 3), {},
                    "sanitize drops an ASCII answer");
    checkCandidates(sanitize("你好abc", "ni'hao", 3), {},
                    "sanitize drops a mixed answer");
    checkCandidates(sanitize("nihao", "ni'hao", 3), {},
                    "sanitize drops an echoed pinyin");

    // Blank lines and empty answers are not candidates.
    checkCandidates(sanitize("\n\n你好\n\n", "ni'hao", 3), {"你好"},
                    "sanitize skips blank lines");
    checkCandidates(sanitize("", "ni'hao", 3), {},
                    "sanitize handles an empty answer");

    // maxCandidates of 0 means unlimited, used by tests and by callers that
    // clamp elsewhere.
    checkCandidates(sanitize("你好\n你好吗", "ni'hao", 0), {"你好"},
                    "sanitize with maxCandidates 0 still filters by length");
}

} // namespace

int main() {
    testRenderPrompt();
    testBuildRequest();
    testParseResponse();
    testCountSyllables();
    testIsAllHan();
    testIsPlainPinyin();
    testSanitize();

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "All tests passed\n";
    return EXIT_SUCCESS;
}
