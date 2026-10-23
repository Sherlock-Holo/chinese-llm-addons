/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "llmbackend.h"
#include <cstdint>
#include <fcitx-utils/utf8.h>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <unordered_set>

namespace fcitx::chinesellm {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

/// Characters a model likes to wrap its answer in. ASCII only: comparing the
/// last byte of a UTF-8 string against an ASCII set is safe, whereas doing the
/// same with a multi-byte set would match a trailing byte of an unrelated Han
/// character and silently eat it.
constexpr std::string_view kNoise = "\"'`*";

/// Whether a code point is trailing punctuation that a bare pinyin key never
/// wants. A user who wants a full stop types it separately.
bool isTrailingPunct(uint32_t chr) {
    switch (chr) {
    case U'。':
    case U'．':
    case U'.':
    case U',':
    case U'，':
    case U'、':
    case U';':
    case U'；':
    case U':':
    case U'：':
    case U'!':
    case U'！':
    case U'?':
    case U'？':
        return true;
    default:
        return false;
    }
}

/// Drop a trailing code point if it is punctuation. Operates on code points,
/// not bytes, so a multi-byte character is never truncated.
std::string_view stripTrailingPunct(std::string_view line) {
    while (true) {
        line = trim(line);
        if (line.empty()) {
            return line;
        }
        // Decode only the final code point, rather than rescanning the entire
        // prefix for every punctuation mark. Even a response-sized suffix is
        // stripped in linear time. Invalid UTF-8 is left for validation below.
        auto begin = std::prev(line.end());
        while (begin != line.begin() &&
               (static_cast<unsigned char>(*begin) & 0xC0) == 0x80) {
            --begin;
        }
        uint32_t chr = utf8::INVALID_CHAR;
        const auto end = utf8::getNextChar(begin, line.end(), &chr);
        if (end != line.end() || !isTrailingPunct(chr)) {
            return line;
        }
        line.remove_suffix(static_cast<size_t>(line.end() - begin));
    }
}

std::string_view stripNoise(std::string_view line) {
    line = trim(line);
    // Leading and trailing ASCII wrappers. Only one byte is ever removed, which
    // is correct because these are ASCII.
    while (!line.empty() &&
           kNoise.find(line.front()) != std::string_view::npos) {
        line.remove_prefix(1);
        line = trim(line);
    }
    while (!line.empty() &&
           kNoise.find(line.back()) != std::string_view::npos) {
        line.remove_suffix(1);
        line = trim(line);
    }
    return stripTrailingPunct(line);
}

bool isHanCodePoint(uint32_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF)      // CJK Unified Ideographs
           || (c >= 0x3400 && c <= 0x4DBF)   // Extension A
           || (c >= 0x20000 && c <= 0x2FA1F) // Extensions B..F, compatibility
           || (c >= 0xF900 && c <= 0xFAFF);  // CJK Compatibility Ideographs
}

/// CJK punctuation we tolerate inside a candidate, e.g. the interpunct in a
/// transliterated name.
bool isAllowedCjkPunct(uint32_t c) {
    switch (c) {
    case 0x00B7: // ·
    case 0x2027: // ‧
    case 0x30FB: // ・
    case 0x2018:
    case 0x2019:
    case 0x201C:
    case 0x201D:
    case 0x3001: // 、
    case 0x3002: // 。
    case 0xFF0C: // ，
        return true;
    default:
        return false;
    }
}

/// Validate candidate text and count only Han, not tolerated punctuation.
std::optional<size_t> countHan(std::string_view text) {
    size_t count = 0;
    auto iter = text.begin();
    while (iter != text.end()) {
        uint32_t chr = utf8::INVALID_CHAR;
        iter = utf8::getNextChar(iter, text.end(), &chr);
        if (chr == utf8::INVALID_CHAR || chr == utf8::NOT_ENOUGH_SPACE) {
            return std::nullopt;
        }
        if (isHanCodePoint(chr)) {
            ++count;
        } else if (!isAllowedCjkPunct(chr)) {
            return std::nullopt;
        }
    }
    return count;
}

std::vector<std::string_view> splitPinyin(std::string_view pinyin) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i <= pinyin.size(); i++) {
        if (i == pinyin.size() || pinyin[i] == '\'' || pinyin[i] == ' ') {
            if (i > start) {
                out.push_back(pinyin.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    return out;
}

} // namespace

std::string renderPrompt(std::string_view promptTemplate,
                         std::string_view pinyin, std::string_view context) {
    std::string out;
    out.reserve(promptTemplate.size() + pinyin.size() + context.size());
    size_t i = 0;
    while (i < promptTemplate.size()) {
        if (promptTemplate.compare(i, 9, "{context}") == 0) {
            out.append(context);
            i += 9;
        } else if (promptTemplate.compare(i, 8, "{pinyin}") == 0) {
            out.append(pinyin);
            i += 8;
        } else {
            out.push_back(promptTemplate[i]);
            i += 1;
        }
    }
    return out;
}

RequestSpec buildRequest(const BackendConfig &config, std::string_view pinyin,
                         std::string_view context) {
    RequestSpec spec;
    spec.url = config.endpoint;

    nlohmann::json body;
    body["model"] = config.model;
    body["stream"] = false;
    body["temperature"] = config.temperature;
    body["max_tokens"] = config.maxTokens;
    body["messages"] = nlohmann::json::array(
        {{{"role", "system"}, {"content", config.systemPrompt}},
         {{"role", "user"},
          {"content", renderPrompt(config.promptTemplate, pinyin, context)}}});
    spec.body = body.dump();

    spec.headers.emplace_back("Content-Type: application/json");
    spec.headers.emplace_back("Accept: application/json");
    // An empty key means a local server that does not authenticate at all.
    // Sending "Bearer " would make llama.cpp and ollama reject the request.
    if (!config.apiKey.empty()) {
        spec.headers.emplace_back("Authorization: Bearer " + config.apiKey);
    }
    return spec;
}

std::string parseResponse(std::string_view body) {
    std::string content;
    parseResponse(body, content);
    return content;
}

bool parseResponse(std::string_view body, std::string &content) {
    content.clear();
    body = trim(body);
    if (body.empty()) {
        return false;
    }
    // Trim to the outermost braces so that a fenced block or trailing newline
    // still parses.
    const auto first = body.find('{');
    const auto last = body.rfind('}');
    if (first == std::string_view::npos || last == std::string_view::npos ||
        last < first) {
        return false;
    }
    body = body.substr(first, last - first + 1);

    try {
        const auto jv = nlohmann::json::parse(body);
        const auto &choices = jv.at("choices");
        if (!choices.is_array() || choices.empty()) {
            return false;
        }
        const auto &choice = choices.at(0);
        const auto finish = choice.find("finish_reason");
        // A token-limited answer can still be valid JSON containing plausible
        // Han text, but it is not safe to commit. Other explicit non-stop
        // reasons (e.g. content_filter/tool_calls) are not completed text
        // either.
        if (finish != choice.end() && !finish->is_null() &&
            (!finish->is_string() || finish->get<std::string>() != "stop")) {
            return false;
        }
        const auto &value = choice.at("message").at("content");
        if (!value.is_string()) {
            return false;
        }
        content = value.get<std::string>();
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

size_t countSyllables(std::string_view pinyin) {
    const auto parts = splitPinyin(pinyin);
    if (parts.empty()) {
        return 0;
    }
    // A single syllable with no separator is indistinguishable from an
    // unseparated multi-syllable string, so no length check is possible.
    if (parts.size() == 1) {
        return 0;
    }
    return parts.size();
}

bool isAllHan(std::string_view text) {
    const auto count = countHan(text);
    // Punctuation alone is not an answer.
    return count && *count != 0;
}

bool isPlainPinyin(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    bool sawLetter = false;
    for (char c : text) {
        if (c >= 'a' && c <= 'z') {
            sawLetter = true;
        } else if (c < 'A' || c > 'Z') {
            if (c != '\'' && c != ' ') {
                return false;
            }
        }
    }
    return sawLetter;
}

std::vector<std::string> sanitize(std::string_view content,
                                  std::string_view pinyin,
                                  size_t maxCandidates) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    const size_t expectedLength = countSyllables(pinyin);

    // Models routinely ignore "single line" and emit several readings. Take
    // each line as a candidate, in the model's own ranking order.
    size_t pos = 0;
    while (pos <= content.size()) {
        if (maxCandidates && result.size() >= maxCandidates) {
            break;
        }
        const auto eol = content.find('\n', pos);
        const auto raw = content.substr(pos, eol == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : eol - pos);
        pos = eol == std::string_view::npos ? content.size() + 1 : eol + 1;

        auto line = stripNoise(raw);
        if (line.empty()) {
            continue;
        }
        // Reject anything that is not Han text: this catches the model
        // answering in English, echoing the pinyin back, or emitting digits,
        // any of which would be committed verbatim if we let it through.
        const auto hanCount = countHan(line);
        if (!hanCount || *hanCount == 0) {
            continue;
        }
        // When the syllable count is known, the Han count must match
        // exactly. The pinyin engine checks this itself only to decide whether
        // to *learn* the word (pinyin.cpp:263 in fcitx5-chinese-addons); the
        // commit path is unconditional, so an extra character here would land
        // in the user's document carrying a silently wrong reading.
        if (expectedLength != 0 && *hanCount != expectedLength) {
            continue;
        }
        // Deduplicate before applying the limit, preserving the model's order.
        // A repeated answer must not crowd out another valid candidate.
        auto [entry, inserted] = seen.emplace(line);
        if (inserted) {
            result.push_back(*entry);
        }
    }
    return result;
}

} // namespace fcitx::chinesellm
