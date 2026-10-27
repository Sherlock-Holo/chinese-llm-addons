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
    checkCandidates(sanitize("你好。 ，！? ", "ni'hao", 3), {"你好"},
                    "sanitize strips mixed trailing punctuation and spaces");
    checkCandidates(sanitize("你好。 ，！? \t", "ni'hao", 3), {},
                    "sanitize rejects trailing control bytes before trimming");

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

    // A pure-Han length mismatch still means the model invented or dropped a
    // character. Mixed-language candidates are covered separately below.
    checkCandidates(sanitize("你好吗", "ni'hao", 3), {},
                    "sanitize drops a too-long candidate");
    checkCandidates(sanitize("你", "ni'hao", 3), {},
                    "sanitize drops a too-short candidate");

    // Without separators the syllable count is unknown, so the pure-Han
    // candidate survives without a length check.
    checkCandidates(sanitize("你换手机", "nhsj", 3), {"你换手机"},
                    "sanitize skips the length check for initials");

    // Safe English and mixed answers are allowed even for separated pinyin.
    checkCandidates(sanitize("hello", "ni'hao", 3), {"hello"},
                    "sanitize accepts an ASCII answer distinct from the keys");
    checkCandidates(sanitize("你好abc", "ni'hao", 3), {"你好abc"},
                    "sanitize accepts a mixed answer");
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

void testSanitizeMixed() {
    const std::vector<std::string> candidates{
        "帮我push一下",
        "你好，world!",
        "Hello, world!",
        "\"Hello, world!\"",
        "“帮我push一下”",
        "don't panic",
        "café",
        "cafe\u0301",
        "版本Ａ２",
        "C++",
        "C#",
        "hello!",
        "2026",
        "版本2.0发布!",
        "😀",
        "☀\uFE0F",
        "☁\uFE0E",
        "‼\uFE0F",
        "⁉\uFE0F",
        "™\uFE0F",
        "ℹ\uFE0F",
        "说明ℹ\uFE0F",
        "👍🏽",
        "👩\u200D💻",
        "👨\u200D👩\u200D👧\u200D👦",
        "🇨🇳",
        "1\uFE0F\u20E3",
        "#\uFE0F\u20E3",
        "*\u20E3",
        "\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E"
        "\U000E0067\U000E007F",
        "\U0001F3F4\U000E0067\U000E0062\U000E0073\U000E0063"
        "\U000E0074\U000E007F",
        "\U0001F3F4\U000E0067\U000E0062\U000E0077\U000E006C"
        "\U000E0073\U000E007F",
        "帮我push一下 👍🏽 👩\u200D💻 🇨🇳 1\uFE0F\u20E3"};
    for (const auto &candidate : candidates) {
        checkCandidates(sanitize(candidate, "bang'wo'push'yi'xia", 3),
                        {candidate},
                        "sanitize preserves mixed/English/digit/emoji bytes: " +
                            candidate);
    }
    checkCandidates(sanitize("你hello", "ni'hao'ma", 3), {"你hello"},
                    "mixed answer skips too-short Han count");
    checkCandidates(sanitize("你好世界hello", "ni'hao", 3), {"你好世界hello"},
                    "mixed answer skips too-long Han count");
    checkCandidates(sanitize("你1", "ni'hao'ma", 3), {"你1"},
                    "digit answer skips too-short Han count");
    checkCandidates(sanitize("你好世界😀", "ni'hao", 3), {"你好世界😀"},
                    "emoji answer skips too-long Han count");
    checkCandidates(sanitize("你好，世界", "ni'hao'shi'jie", 3),
                    {"你好，世界"},
                    "pure Han with internal CJK punctuation keeps length check");
    checkCandidates(sanitize("你好，世界", "ni'hao", 3), {},
                    "CJK punctuation does not exempt pure-Han length mismatch");
    checkCandidates(sanitize("帮我push一下\r\nHello, world!\r\n👍🏽\r\n",
                             "bang'wo'push'yi'xia", 0),
                    {"帮我push一下", "Hello, world!", "👍🏽"},
                    "sanitize accepts CRLF candidate separators byte-exact");
    checkCandidates(sanitize("hello!\nhello!\nC++\n😀", "ni'hao", 2),
                    {"hello!", "C++"},
                    "mixed candidates remain deduplicated and capped");

    for (const auto &echo : {"nihao", "NIHAO", "Ni Hao", "NI'HAO",
                             "N I ' H A O", "\"NI HAO\"", "nihao!",
                             "\"NI HAO\".", "**ni hao**!"}) {
        checkCandidates(sanitize(echo, "ni'hao", 3), {},
                        "sanitize rejects normalized pinyin echo: " +
                            std::string(echo));
    }
    checkCandidates(sanitize("ni'hao", "NI HAO", 3), {},
                    "echo comparison normalizes input keys as well as output");
    checkCandidates(sanitize("hello", "hello", 3), {},
                    "indistinguishable pure-English input/output defaults to "
                    "echo rejection");
    checkCandidates(sanitize("HeLLo", "he'llo", 3), {},
                    "English echo is rejected after case/apostrophe folding");
    checkCandidates(sanitize("NI HAO\nhello!\n帮我push一下", "ni'hao", 3),
                    {"hello!", "帮我push一下"},
                    "echo filtering does not suppress other safe candidates");
}

void testSanitizeUnsafe() {
    // Reject ill-formed UTF-8 and non-scalar encodings before any edge stripping.
    const std::vector<std::string> invalidUtf8{
        std::string("\x80", 1), std::string("\xbf", 1),
        std::string("\xc0\x80", 2), std::string("\xc1\xbf", 2),
        std::string("\xe0\x80\xaf", 3), std::string("\xed\xa0\x80", 3),
        std::string("\xed\xbf\xbf", 3),
        std::string("\xf0\x80\x80\xaf", 4),
        std::string("\xf4\x90\x80\x80", 4),
        std::string("\xf5\x80\x80\x80", 4), std::string("\xfe", 1),
        std::string("\xff", 1), std::string("\xc2", 1),
        std::string("\xe3\x81", 2), std::string("\xf0\x9f\x98", 3),
        std::string("\xe3\x28\x81", 3)};
    for (size_t i = 0; i < invalidUtf8.size(); ++i) {
        const auto &bytes = invalidUtf8[i];
        for (const auto &candidate : {bytes + "hello!", "hello!" + bytes,
                                      "你" + bytes + "好"}) {
            checkCandidates(sanitize(candidate, "ni'hao", 3), {},
                            "sanitize rejects invalid UTF-8 case " +
                                std::to_string(i));
        }
    }

    // LF separates candidates; a CR is allowed only as part of CRLF. All other
    // C0 bytes, including a leading/trailing tab, must never become invisible.
    for (unsigned int code = 0; code <= 0x1f; ++code) {
        if (code == '\n') {
            continue;
        }
        const std::string control(1, static_cast<char>(code));
        for (const auto &candidate : {control + "hello!", "hello!" + control,
                                      "hello" + control + "world!"}) {
            checkCandidates(sanitize(candidate, "ni'hao", 3), {},
                            "sanitize rejects C0 control " +
                                std::to_string(code) + " even at an edge");
        }
    }
    for (unsigned int code = 0x7f; code <= 0x9f; ++code) {
        const std::string control = code == 0x7f
                                        ? std::string(1, '\x7f')
                                        : std::string("\xc2", 1) +
                                              static_cast<char>(code);
        checkCandidates(sanitize(control + "hello!", "ni'hao", 3), {},
                        "sanitize rejects DEL/C1 prefix " +
                            std::to_string(code));
        checkCandidates(sanitize("hello!" + control, "ni'hao", 3), {},
                        "sanitize rejects DEL/C1 suffix " +
                            std::to_string(code));
        checkCandidates(sanitize("你" + control + "好", "ni'hao", 3), {},
                        "sanitize rejects DEL/C1 inside Han " +
                            std::to_string(code));
    }

    const std::vector<std::string> unsafeComponents{
        "\u200B", "\u200C", "\u200D", "\u200E", "\u200F", "\u202A",
        "\u202B", "\u202C", "\u202D", "\u202E", "\u2066", "\u2067",
        "\u2068", "\u2069", "\uFEFF", "\uFE0E", "\uFE0F", "🏽",
        "\U000E0067", "\U000E007F", "\u20E3"};
    for (const auto &component : unsafeComponents) {
        checkCandidates(sanitize(component, "ni'hao", 3), {},
                        "sanitize rejects isolated invisible/emoji component");
        checkCandidates(sanitize(component + "hello!", "ni'hao", 3), {},
                        "sanitize rejects non-emoji format prefix");
        checkCandidates(sanitize("你好" + component, "ni'hao", 3), {},
                        "sanitize rejects non-emoji format suffix");
    }
    for (const auto &candidate : {"hello\u200Dworld", "你\uFE0F好",
                                  "👩\u200D", "\u200D💻", "A\u20E3",
                                  "\U0001F3F4\U000E0067\U000E0062",
                                  "\U0001F3F4\U000E007F",
                                  "\U0001F3F4\U000E0073\U000E0065"
                                  "\U000E0063\U000E0072\U000E0065"
                                  "\U000E0074\U000E007F",
                                  "\U0001F3F4\U000E0067\U000E0062"
                                  "\U000E0078\U000E0078\U000E0078"
                                  "\U000E007F"}) {
        checkCandidates(sanitize(candidate, "ni'hao", 3), {},
                        "sanitize rejects malformed emoji/format sequence");
    }
    checkCandidates(sanitize("。。。\n!!!\n   ", "ni'hao", 3), {},
                    "sanitize still rejects punctuation-only output");
    checkCandidates(sanitize("hello\tworld\nHello, world!", "ni'hao", 3),
                    {"Hello, world!"},
                    "bad candidate is rejected without losing a later safe line");
}

void testSanitizeDetailed() {
    const std::string raw =
        "帮我push一下\n你好。\nNI HAO\n你\nhello\tworld\n。。。\n\n你好abc";
    const auto detailed = sanitizeDetailed(raw, "ni'hao", 0);
    checkCandidates(detailed.candidates, {"帮我push一下", "你好", "你好abc"},
                    "detailed sanitizer preserves accepted mixed/Han semantics");
    checkCandidates(detailed.candidates, sanitize(raw, "ni'hao", 0),
                    "legacy sanitize is a detailed-result wrapper");
    const std::vector<std::string> reasons{
        "key_echo", "han_length_mismatch", "invalid_text",
        "empty_or_punctuation"};
    checkEq(detailed.rejected.size(), reasons.size(),
            "detailed sanitizer records every rejected line in order");
    for (size_t i = 0; i < detailed.rejected.size() && i < reasons.size(); ++i) {
        checkEq(detailed.rejected[i].reason, reasons[i],
                "detailed sanitizer reason for line " + std::to_string(i));
    }
    if (detailed.rejected.size() >= 3) {
        checkEq(detailed.rejected[0].text, std::string("NI HAO"),
                "diagnostic text preserves raw key echo rather than normalizing");
        checkEq(detailed.rejected[2].text, std::string("hello\\x09world"),
                "diagnostic text escapes controls readably");
    }
    const auto invalid = sanitizeDetailed(std::string("你") + '\x80' + "好",
                                          "ni'hao", 0);
    check(invalid.candidates.empty() && invalid.rejected.size() == 1 &&
              invalid.rejected[0].reason == "invalid_text" &&
              invalid.rejected[0].text == "你\\x80好",
          "malformed UTF-8 produces readable rejection rather than repaired text");
    const auto invalidScalar =
        sanitizeDetailed(std::string("hello\xed\xa0\x80", 8), "ni'hao", 0);
    check(invalidScalar.rejected.size() == 1 &&
              invalidScalar.rejected[0].text == "hello\\xed\\xa0\\x80",
          "diagnostic UTF-8 scalar errors are escaped byte-by-byte");
    const auto controls = sanitizeDetailed("你好\u202e\n你好\u0085\n你好!世界",
                                           "ni'hao", 0);
    check(controls.candidates.empty() && controls.rejected.size() == 3,
          "invisible Unicode and unsupported pure-Han punctuation stay rejected");
    for (const auto &rejected : controls.rejected) {
        checkEq(rejected.reason, std::string("invalid_text"),
                "invalid payload/control uses fixed invalid_text code");
    }

    const auto capped = sanitizeDetailed("hello!\nC++\nNI HAO\n你", "ni'hao", 1);
    checkCandidates(capped.candidates, {"hello!"},
                    "accepted candidate cap retains original ranking");
    check(capped.rejected.size() == 2 &&
              capped.rejected[0].reason == "key_echo" &&
              capped.rejected[1].reason == "han_length_mismatch",
          "accepted cap does not hide later rejection diagnostics");
    const auto duplicates = sanitizeDetailed("你好\n你好\n拟好", "ni'hao", 2);
    checkCandidates(duplicates.candidates, {"你好", "拟好"},
                    "detailed sanitizer deduplicates before accepted cap");
    check(duplicates.rejected.empty(),
          "duplicate safe lines are not falsely reported as rejected text");

    std::string many;
    for (size_t i = 0; i < 50; ++i) {
        many += "NI HAO\n";
    }
    many += "Hello, world!";
    const auto bounded = sanitizeDetailed(many, "ni'hao", 0);
    checkEq(bounded.rejected.size(), size_t{32},
            "at most 32 rejected lines are retained");
    checkCandidates(bounded.candidates, {"Hello, world!"},
                    "rejection diagnostic cap does not suppress later acceptance");

    // 341 Han scalars fill 1023 bytes. Neither the next scalar nor an escape
    // may be split just to use the final byte of the diagnostic budget.
    std::string longHan;
    for (size_t i = 0; i < 500; ++i) {
        longHan += "你";
    }
    const auto longRejected = sanitizeDetailed(longHan, "ni'hao", 0);
    check(longRejected.rejected.size() == 1 &&
              longRejected.rejected[0].text.size() == 1023 &&
              isAllHan(longRejected.rejected[0].text),
          "rejected text is capped on a valid UTF-8 scalar boundary");
    const auto longInvalid =
        sanitizeDetailed(std::string(1022, 'a') + '\x80', "ni'hao", 0);
    check(longInvalid.rejected.size() == 1 &&
              longInvalid.rejected[0].text == std::string(1022, 'a'),
          "bounded diagnostics never retain a partial byte escape");
    const auto oversized =
        sanitizeDetailed(std::string(MAX_RESPONSE_BYTES + 1, 'x'), "ni'hao", 0);
    check(oversized.candidates.empty() && oversized.rejected.size() == 1 &&
              oversized.rejected[0].reason == "response_too_large" &&
              oversized.rejected[0].text.size() == 1024,
          "oversized responses report one bounded response_too_large reject");
    const auto empty = sanitizeDetailed("", "ni'hao", 0);
    check(empty.candidates.empty() && empty.rejected.empty(),
          "empty completion has no accepted or rejected payload");
    const auto blank = sanitizeDetailed("\n \n   \r\n\r\n  ", "ni'hao", 0);
    check(blank.candidates.empty() && blank.rejected.empty(),
          "empty and ordinary-space-only LF/CRLF lines are skipped");
    const auto blankMixed =
        sanitizeDetailed(" \n\nhello!\r\n  \r\n", "ni'hao", 0);
    checkCandidates(blankMixed.candidates, {"hello!"},
                    "blank lines do not affect accepted candidates");
    check(blankMixed.rejected.empty(),
          "blank lines around valid output do not produce false rejects");
    const auto blankControls = sanitizeDetailed("\t\n\r\n \r", "ni'hao", 0);
    check(blankControls.candidates.empty() && blankControls.rejected.size() == 2 &&
              blankControls.rejected[0].reason == "invalid_text" &&
              blankControls.rejected[1].reason == "invalid_text",
          "blank-looking tab and bare CR remain invalid_text, not empty");
    const auto punctuation = sanitizeDetailed("。。。\n!!!", "ni'hao", 0);
    check(punctuation.candidates.empty() && punctuation.rejected.size() == 2 &&
              punctuation.rejected[0].reason == "empty_or_punctuation" &&
              punctuation.rejected[1].reason == "empty_or_punctuation",
          "punctuation payload still reports rejection rather than empty output");
    const auto crlf = sanitizeDetailed("你好\r\n", "ni'hao", 0);
    checkCandidates(crlf.candidates, {"你好"},
                    "detailed diagnostics retain CRLF acceptance");
    check(crlf.rejected.empty(),
          "trailing CRLF does not create a spurious empty rejection");
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
    testSanitizeMixed();
    testSanitizeUnsafe();
    testSanitizeDetailed();

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "All tests passed\n";
    return EXIT_SUCCESS;
}
