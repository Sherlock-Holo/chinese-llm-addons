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

bool isLatinLetter(uint32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') ||
           (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7) ||
           (c >= 0x1E00 && c <= 0x1EFF) || (c >= 0xFF21 && c <= 0xFF3A) ||
           (c >= 0xFF41 && c <= 0xFF5A);
}

bool isEmojiBase(uint32_t c) {
    // Visible emoji/symbol ranges, not arbitrary supplementary-plane text.
    // Skin tones are suffixes, not standalone emoji bases.
    if (c >= 0x1F3FB && c <= 0x1F3FF) {
        return false;
    }
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) ||
           (c >= 0x2194 && c <= 0x2199) || c == 0x21A9 || c == 0x21AA ||
           c == 0x231A || c == 0x231B || c == 0x2328 || c == 0x23CF ||
           (c >= 0x23E9 && c <= 0x23F3) || (c >= 0x23F8 && c <= 0x23FA) ||
           c == 0x24C2 || c == 0x25AA || c == 0x25AB || c == 0x25B6 ||
           c == 0x25C0 || (c >= 0x25FB && c <= 0x25FE) || c == 0x2934 ||
           c == 0x2935 || (c >= 0x2B05 && c <= 0x2B07) || c == 0x2B1B ||
           c == 0x2B1C || c == 0x2B50 || c == 0x2B55 || c == 0x3030 ||
           c == 0x303D || c == 0x3297 || c == 0x3299 || c == 0x00A9 ||
           c == 0x00AE || c == 0x203C || c == 0x2049 || c == 0x2122 ||
           c == 0x2139;
}

bool isEmojiSelector(uint32_t c) { return c == 0xFE0E || c == 0xFE0F; }

// Consume a visible emoji base with its optional presentation/skin-tone/tag
// suffixes. Format characters are allowed only inside these sequences. In
// particular, a standalone ZWJ, variation selector or tag is never printable
// candidate content merely because an emoji appears elsewhere in the line.
bool consumeEmojiAtom(const std::vector<uint32_t> &chars, size_t &i) {
    if (i == chars.size() || !isEmojiBase(chars[i])) {
        return false;
    }
    const auto base = chars[i++];
    if (i < chars.size() && isEmojiSelector(chars[i])) {
        ++i;
    }
    if (i < chars.size() && chars[i] >= 0x1F3FB && chars[i] <= 0x1F3FF) {
        ++i;
    }
    // The three standardized subdivision flags use BLACK FLAG + their tag
    // identifier + CANCEL TAG. Do not allow arbitrary invisible tag payloads
    // just because a black flag precedes them.
    if (base == 0x1F3F4 && i < chars.size() && chars[i] >= 0xE0020 &&
        chars[i] <= 0xE007E) {
        std::string flag;
        while (i < chars.size() && chars[i] >= 0xE0061 && chars[i] <= 0xE007A &&
               flag.size() < 5) {
            flag.push_back(static_cast<char>(chars[i++] - 0xE0000));
        }
        if ((flag != "gbeng" && flag != "gbsct" && flag != "gbwls") ||
            i == chars.size() || chars[i] != 0xE007F) {
            return false;
        }
        ++i;
    }
    return true;
}

bool isCandidatePunct(uint32_t c) {
    // ASCII space/punctuation, visible typographic/CJK punctuation and
    // fullwidth punctuation. No C0/C1, bidi controls, zero-width spaces,
    // Unicode line separators, BOM or arbitrary format characters.
    return (c >= 0x20 && c <= 0x7E) || isAllowedCjkPunct(c) ||
           (c >= 0x2010 && c <= 0x2027) || c == 0x2030 || c == 0x2032 ||
           c == 0x2033 || c == 0x203B || c == 0x20AC ||
           (c >= 0x3001 && c <= 0x301F) || (c >= 0xFF01 && c <= 0xFF20) ||
           (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65);
}

struct CandidateText {
    size_t hanCount = 0;
    bool hasOtherContent = false; // Latin letters, digits or visible emoji.
};

std::optional<CandidateText> validateCandidate(std::string_view text) {
    std::vector<uint32_t> chars;
    chars.reserve(text.size());
    auto iter = text.begin();
    while (iter != text.end()) {
        uint32_t c = utf8::INVALID_CHAR;
        iter = utf8::getNextChar(iter, text.end(), &c);
        // Fcitx's decoder is also checked for Unicode scalar/noncharacter
        // validity, rather than assuming every decoded integer is text.
        if (!utf8::isValidChar(c) || c > 0x10FFFF ||
            (c >= 0xD800 && c <= 0xDFFF) || (c >= 0xFDD0 && c <= 0xFDEF) ||
            (c & 0xFFFF) >= 0xFFFE) {
            return std::nullopt;
        }
        chars.push_back(c);
    }

    CandidateText result;
    bool latinCombining = false;
    for (size_t i = 0; i < chars.size();) {
        const auto c = chars[i];
        // Keycaps: [0-9#*] + optional VS16 + COMBINING ENCLOSING KEYCAP.
        // Without the enclosure these remain ordinary ASCII, including '*'.
        if ((c >= U'0' && c <= U'9') || c == U'#' || c == U'*') {
            auto end = i + 1;
            if (end < chars.size() && chars[end] == 0xFE0F) {
                ++end;
            }
            if (end < chars.size() && chars[end] == 0x20E3) {
                i = end + 1;
                result.hasOtherContent = true;
                latinCombining = false;
                continue;
            }
        }
        if (isEmojiBase(c)) {
            if (!consumeEmojiAtom(chars, i)) {
                return std::nullopt;
            }
            while (i < chars.size() && chars[i] == 0x200D) {
                ++i;
                if (!consumeEmojiAtom(chars, i)) {
                    return std::nullopt;
                }
            }
            result.hasOtherContent = true;
            latinCombining = false;
            continue;
        }
        if (isHanCodePoint(c)) {
            ++result.hanCount;
            latinCombining = false;
        } else if (isLatinLetter(c)) {
            result.hasOtherContent = true;
            latinCombining = true;
        } else if ((c >= U'0' && c <= U'9') || (c >= 0xFF10 && c <= 0xFF19)) {
            result.hasOtherContent = true;
            latinCombining = false;
        } else if (c >= 0x0300 && c <= 0x036F && c != 0x034F &&
                   latinCombining) {
            // Preserve decomposed Latin accents, but not the invisible CGJ.
        } else if (isCandidatePunct(c)) {
            latinCombining = false;
        } else {
            return std::nullopt;
        }
        ++i;
    }
    return result;
}

std::string normalizedKeys(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        if (c == ' ' || c == '\'') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        result.push_back(c);
    }
    return result;
}

// Diagnostic snippets are bounded independently of the response. Copy complete
// valid UTF-8 scalars only; escape malformed bytes and controls rather than
// letting an unsafe reply corrupt the history display or JSON serialization.
std::string diagnosticText(std::string_view text) {
    constexpr size_t maxBytes = 1024;
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    auto iter = text.begin();
    while (iter != text.end()) {
        uint32_t c = utf8::INVALID_CHAR;
        const auto next = utf8::getNextChar(iter, text.end(), &c);
        const bool valid = utf8::isValidChar(c) && c <= 0x10FFFF &&
                           !(c >= 0xD800 && c <= 0xDFFF) &&
                           !(c >= 0xFDD0 && c <= 0xFDEF) &&
                           (c & 0xFFFF) < 0xFFFE;
        if (!valid || c < 0x20 || (c >= 0x7F && c <= 0x9F)) {
            if (result.size() + 4 > maxBytes) {
                break;
            }
            const auto byte = static_cast<unsigned char>(*iter++);
            result += "\\x";
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0xF]);
        } else {
            const auto bytes = static_cast<size_t>(next - iter);
            if (result.size() + bytes > maxBytes) {
                break;
            }
            result.append(iter, next);
            iter = next;
        }
    }
    return result;
}

void reject(SanitizedCandidates &result, std::string_view text,
            const char *reason) {
    if (result.rejected.size() < 32) {
        result.rejected.push_back({diagnosticText(text), reason});
    }
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

SanitizedCandidates sanitizeDetailed(std::string_view content,
                                     std::string_view pinyin,
                                     size_t maxCandidates) {
    SanitizedCandidates result;
    if (content.size() > MAX_RESPONSE_BYTES) {
        reject(result, content, "response_too_large");
        return result;
    }
    std::unordered_set<std::string> seen;
    const size_t expectedLength = countSyllables(pinyin);
    const auto keys = normalizedKeys(pinyin);

    // Models routinely ignore "single line" and emit several readings. Take
    // each line as a candidate, in the model's own ranking order. Continue past
    // the accepted limit to diagnose rejected lines without changing that list.
    size_t pos = 0;
    while (pos <= content.size()) {
        const auto eol = content.find('\n', pos);
        auto raw = content.substr(pos, eol == std::string_view::npos
                                           ? std::string_view::npos
                                           : eol - pos);
        pos = eol == std::string_view::npos ? content.size() + 1 : eol + 1;
        // LF separates candidates; tolerate CR only as part of an actual CRLF.
        // A bare CR, tab or other control is not whitespace we can erase.
        if (eol != std::string_view::npos && !raw.empty() &&
            raw.back() == '\r') {
            raw.remove_suffix(1);
        }
        // Empty/ordinary-space-only lines are absence of output, not filtered
        // payload. Do not trim controls: tabs and bare CR must still reject.
        if (raw.find_first_not_of(' ') == std::string_view::npos) {
            continue;
        }
        const auto text = validateCandidate(raw);
        if (!text) {
            reject(result, raw, "invalid_text");
            continue;
        }
        if (text->hanCount == 0 && !text->hasOtherContent) {
            reject(result, raw, "empty_or_punctuation");
            continue;
        }

        // Preserve English/mixed/emoji punctuation (C++, C#, hello!, *️⃣) and
        // all emoji sequence bytes. Legacy bare-Han results still shed the
        // model's quotes/full stops before their exact syllable check.
        auto line = text->hasOtherContent ? raw : stripNoise(raw);
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        while (!line.empty() && line.back() == ' ') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            reject(result, raw, "empty_or_punctuation");
            continue;
        }
        if (!text->hasOtherContent) {
            const auto hanCount = countHan(line);
            if (!hanCount) {
                reject(result, raw, "invalid_text");
                continue;
            }
            if (*hanCount == 0) {
                reject(result, raw, "empty_or_punctuation");
                continue;
            }
            if (expectedLength != 0 && *hanCount != expectedLength) {
                reject(result, raw, "han_length_mismatch");
                continue;
            }
        }
        // English keys and English words can be indistinguishable. Reject an
        // exact normalized echo conservatively, not all Latin text. Check a
        // de-noised view only; do not mutate an accepted mixed/English result.
        auto echo = line;
        while (true) {
            const auto stripped = stripNoise(echo);
            if (stripped.size() == echo.size()) {
                break;
            }
            echo = stripped;
        }
        if (!keys.empty() && normalizedKeys(echo) == keys) {
            reject(result, raw, "key_echo");
            continue;
        }
        // Deduplicate before applying the limit, preserving the model's order.
        // A repeated answer must not crowd out another valid candidate.
        if (!maxCandidates || result.candidates.size() < maxCandidates) {
            auto [entry, inserted] = seen.emplace(line);
            if (inserted) {
                result.candidates.push_back(*entry);
            }
        }
    }
    return result;
}

std::vector<std::string> sanitize(std::string_view content,
                                  std::string_view pinyin,
                                  size_t maxCandidates) {
    return sanitizeDetailed(content, pinyin, maxCandidates).candidates;
}

} // namespace fcitx::chinesellm
