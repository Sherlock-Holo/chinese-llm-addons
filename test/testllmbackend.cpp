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

void testSanitizeHan() {
    checkCandidates(sanitize("你好", "ni'hao", 3), {"你好"},
                    "a matching reading is accepted");
    checkCandidates(sanitize("\"你好。\"", "ni'hao", 3), {"你好"},
                    "quotes and a trailing full stop are stripped");
    checkCandidates(sanitize("我们去吃饭吧\n我们去吃饭不\n我们去吃饭呗",
                             "wo'men'qu'chi'fan'ba", 2),
                    {"我们去吃饭吧", "我们去吃饭不"},
                    "lines become ranked candidates up to the limit");
    checkCandidates(sanitize("你好\n你好\n拟好", "ni'hao", 2), {"你好", "拟好"},
                    "duplicates do not use up the limit");
    checkCandidates(sanitize("你好吗\n你", "ni'hao", 3), {},
                    "a pure-Han reading must match the syllable count");
    checkCandidates(sanitize("你换手机", "nhsj", 3), {"你换手机"},
                    "initials have no syllable count to check");
    checkCandidates(sanitize("\n\n你好\n\n", "ni'hao", 3), {"你好"},
                    "blank lines are skipped");
    std::string noisy = "你好";
    while (noisy.size() + 4 <= MAX_RESPONSE_BYTES) {
        noisy += "。 ";
    }
    const auto started = std::chrono::steady_clock::now();
    checkCandidates(sanitize(noisy, "ni'hao", 3), {"你好"},
                    "a response-sized punctuation suffix is stripped");
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
          "stripping punctuation is linear time");
}

void testSanitizeMixed() {
    const std::vector<std::string> accepted{
        "帮我push一下", "你好，world!", "Hello", "C++", "C#", "café",
        "cafe\u0301", "2026", "版本2.0发布!", "😀", "☁\uFE0E", "👍🏽",
        "👩\u200D💻", "🇨🇳", "1\uFE0F\u20E3",
        "\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067"
        "\U000E007F"};
    for (const auto &candidate : accepted) {
        checkCandidates(sanitize(candidate, "bang'wo'push'yi'xia", 3),
                        {candidate}, "accepted byte-exact: " + candidate);
    }
    checkCandidates(sanitize("\"帮我push一下\"", "bang'wo'push'yi'xia", 3),
                    {"帮我push一下"}, "wrapping quotes are stripped from mixed text");
    checkCandidates(sanitize("**帮我push一下**", "bang'wo'push'yi'xia", 3),
                    {"帮我push一下"}, "markdown emphasis is stripped from mixed text");
    checkCandidates(sanitize("你hello", "ni'hao'ma", 3), {"你hello"},
                    "mixed text has no one-Han-per-syllable rule");
    for (const auto &echo : {"nihao", "NI HAO", "\"NI'HAO\"", "nihao!", "**ni hao**"}) {
        checkCandidates(sanitize(echo, "ni'hao", 3), {},
                        std::string("normalized key echo is rejected: ") + echo);
    }
}

// Chat models narrate. Now that Latin text is legitimate, narration must not
// become candidates.
void testSanitizeNarration() {
    const auto chatty = sanitizeDetailed(
        "Sure! Here's the conversion:\n你好", "ni'hao", 0);
    checkCandidates(chatty.candidates, {"你好"}, "an announcement line is not a candidate");
    check(chatty.rejected.size() == 1 && chatty.rejected[0].reason == "markup",
          "the announcement is reported");
    for (const auto &reasoning : {
             "<think>\nOkay, the user typed nihao.\n</think>\n你好",
             "<think>\n\n</think>\n\n你好", "</think>\n你好"}) {
        checkCandidates(sanitize(reasoning, "nihao", 3), {"你好"},
                        "inlined reasoning is removed");
    }
    checkCandidates(sanitize("<think>\nstill thinking", "nihao", 3), {},
                    "an unfinished reasoning block yields no candidate");
    checkCandidates(sanitize("<br>\n</s>", "nihao", 3), {},
                    "markup tags are rejected");
    checkCandidates(sanitize("1. 你好\n2) 拟好\n- 你好\n* 拟好", "nihao", 5), {},
                    "enumerated and bulleted lines are rejected");
    checkCandidates(sanitize("3.14\n1,000", "shu'zi", 3), {"3.14", "1,000"},
                    "numbers are not list markers");
    checkCandidates(
        sanitize("你好，我是一个输入法助手，很高兴为你服务，请问还有什么可以帮你的吗",
                 "nihao", 3),
        {}, "an answer far longer than its keys is rejected");
    checkCandidates(sanitize("Hello, world! C++ C#", "ying'wen", 3), {},
                    "Latin text may not outgrow the typed letters");
}

void testSanitizeUnsafe() {
    const std::vector<std::string> invalidUtf8{
        std::string("\x80", 1), std::string("\xc0\x80", 2),
        std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
        std::string("\xff", 1), std::string("\xe3\x81", 2)};
    for (const auto &bytes : invalidUtf8) {
        for (const auto &candidate : {bytes + "hello", "hello" + bytes,
                                      "你" + bytes + "好"}) {
            checkCandidates(sanitize(candidate, "ni'hao", 3), {},
                            "invalid UTF-8 is rejected, never repaired");
        }
    }
    for (const auto &control :
         {"\t", "\r", "\x1b", "\x7f", "\xc2\x85"}) {
        checkCandidates(sanitize(std::string("hello") + control + "world", "ni'hao", 3),
                        {}, "control characters are rejected");
        checkCandidates(sanitize(std::string("hello") + control, "ni'hao", 3), {},
                        "a control at the edge is not trimmed away");
    }
    for (const auto &component : {"\u200B", "\u200D", "\u202E", "\u2066",
                                  "\uFEFF", "\uFE0F", "\U000E007F", "\u20E3"}) {
        checkCandidates(sanitize(std::string("hello") + component, "ni'hao", 3), {},
                        "invisible and dangling format characters are rejected");
    }
    for (const auto &candidate : {"👩\u200D", "\u200D💻",
                                  "\U0001F3F4\U000E0067\U000E0062",
                                  "\U0001F3F4\U000E0073\U000E0065\U000E0063"
                                  "\U000E0072\U000E0065\U000E0074\U000E007F"}) {
        checkCandidates(sanitize(candidate, "ni'hao", 3), {},
                        "malformed emoji sequences are rejected");
    }
    checkCandidates(sanitize("。。。\n!!!\n   ", "ni'hao", 3), {},
                    "punctuation-only output is rejected");
    checkCandidates(sanitize("hello\tworld\n你好", "ni'hao", 3), {"你好"},
                    "an unsafe line does not hide a later safe one");
}

void testSanitizeDetailed() {
    const std::string raw = "帮我push一下\n你好。\nNI HAO\n你\nhello\tworld\n。。。\n\n你好abc";
    const auto detailed = sanitizeDetailed(raw, "ni'hao", 0);
    checkCandidates(detailed.candidates, {"帮我push一下", "你好", "你好abc"},
                    "detailed result keeps accepted candidates in order");
    checkCandidates(detailed.candidates, sanitize(raw, "ni'hao", 0),
                    "sanitize wraps the detailed result");
    const std::vector<std::string> reasons{"key_echo", "han_length_mismatch",
                                           "invalid_text", "empty_or_punctuation"};
    check(detailed.rejected.size() == reasons.size(),
          "every rejected line is reported");
    for (size_t i = 0; i < detailed.rejected.size() && i < reasons.size(); ++i) {
        checkEq(detailed.rejected[i].reason, reasons[i],
                "rejection reason for line " + std::to_string(i));
    }
    if (detailed.rejected.size() == reasons.size()) {
        checkEq(detailed.rejected[2].text, std::string("hello\\x09world"),
                "controls are shown as readable escapes");
    }
    const auto invalid = sanitizeDetailed(std::string("你") + '\x80' + "好", "ni'hao", 0);
    check(invalid.rejected.size() == 1 && invalid.rejected[0].text == "你\\x80好",
          "malformed bytes are escaped in diagnostics");

    std::string many;
    for (size_t i = 0; i < 50; ++i) {
        many += "NI HAO\n";
    }
    many += "Hello";
    const auto bounded = sanitizeDetailed(many, "ni'hao", 0);
    check(bounded.rejected.size() == 32, "at most 32 rejections are kept");
    checkCandidates(bounded.candidates, {"Hello"},
                    "the rejection cap does not hide a later candidate");
    std::string longHan;
    for (size_t i = 0; i < 500; ++i) {
        longHan += "你";
    }
    const auto longRejected = sanitizeDetailed(longHan, "ni'hao", 0);
    check(longRejected.rejected.size() == 1 &&
              longRejected.rejected[0].text.size() == 1023 &&
              isAllHan(longRejected.rejected[0].text),
          "diagnostic text is cut on a character boundary");
    const auto oversized =
        sanitizeDetailed(std::string(MAX_RESPONSE_BYTES + 1, 'x'), "ni'hao", 0);
    check(oversized.candidates.empty() && oversized.rejected.size() == 1 &&
              oversized.rejected[0].reason == "response_too_large",
          "an oversized reply is one bounded rejection");
    check(sanitizeDetailed("", "ni'hao", 0).rejected.empty() &&
              sanitizeDetailed("\n \n   \r\n", "ni'hao", 0).rejected.empty(),
          "empty and blank replies are not reported as rejected");
}

void testPredicates() {
    check(countSyllables("ni'hao") == 2 && countSyllables("nihao") == 0,
          "syllables are only countable with separators");
    check(isAllHan("你好") && !isAllHan("你好a") && !isAllHan("。"),
          "isAllHan accepts only Han and tolerated punctuation");
    check(isPlainPinyin("ni'hao") && isPlainPinyin("ni hao") &&
              !isPlainPinyin("你好 ni") && !isPlainPinyin("ni1"),
          "plain pinyin has no Han or digits");
}

} // namespace

int main() {
    testRenderPrompt();
    testBuildRequest();
    testParseResponse();
    testPredicates();
    testSanitizeHan();
    testSanitizeMixed();
    testSanitizeNarration();
    testSanitizeUnsafe();
    testSanitizeDetailed();

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "All tests passed\n";
    return EXIT_SUCCESS;
}
