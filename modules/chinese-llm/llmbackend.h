/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef _CHINESE_LLM_LLMBACKEND_H_
#define _CHINESE_LLM_LLMBACKEND_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fcitx::chinesellm {

/// Upper bound on a response body we are willing to buffer. A chat completion
/// reply carrying usage statistics is a few hundred bytes to a few kilobytes;
/// the long tail is only ever noise, and the input method must never buffer an
/// unbounded amount of data on a server's behalf.
constexpr size_t MAX_RESPONSE_BYTES = 64 * 1024;

/// Upper bound on the pinyin we will send. The model this was built for accepts
/// 40 keys; anything beyond that is a paste or a stuck key, not typing.
constexpr size_t MAX_PINYIN_LENGTH = 40;

struct BackendConfig {
    std::string endpoint;
    std::string model;
    std::string apiKey;
    std::string systemPrompt;
    std::string promptTemplate;
    double temperature = 0.0;
    int maxTokens = 64;
    int timeoutSeconds = 5;
};

struct RequestSpec {
    std::string url;
    std::string body;
    std::vector<std::string> headers;
};

/// Render the user message by substituting {context} and {pinyin}.
std::string renderPrompt(std::string_view promptTemplate,
                         std::string_view pinyin, std::string_view context);

/// Build an OpenAI compatible chat completions request.
RequestSpec buildRequest(const BackendConfig &config, std::string_view pinyin,
                         std::string_view context);

/// Extract choices[0].message.content from a chat completions reply.
///
/// Returns an empty string on any parse failure, when content is missing or is
/// not a string, or when the reply is explicitly incomplete (e.g. token limit).
std::string parseResponse(std::string_view body);

/// Parse a complete response, distinguishing malformed replies from valid empty
/// content. Clears content and returns false on failure. Missing/null
/// finish_reason is tolerated for local OpenAI-compatible servers; an explicit
/// finish_reason must be "stop".
bool parseResponse(std::string_view body, std::string &content);

struct RejectedCandidate {
    std::string text;
    std::string reason;
};

struct SanitizedCandidates {
    std::vector<std::string> candidates;
    std::vector<RejectedCandidate> rejected;
};

/// Sanitize with bounded per-line diagnostics. Rejection reasons are fixed codes:
/// invalid_text, empty_or_punctuation, han_length_mismatch, key_echo and
/// response_too_large. At most 32 rejected lines are retained, each at most 1024
/// bytes of UTF-8-safe text (invalid bytes and controls use readable escapes).
/// Empty/ordinary-ASCII-space-only lines are skipped, not reported as rejects.
/// Candidate limits do not prevent recording rejected lines later in the reply.
SanitizedCandidates sanitizeDetailed(std::string_view content,
                                     std::string_view pinyin,
                                     size_t maxCandidates);

/// Turn raw model output into candidates that are safe to commit.
///
/// This is the only place that decides whether model output may reach the
/// candidate list. Accepts Han/Latin text, digits and visible emoji sequences,
/// preserving mixed-text punctuation. Invalid UTF-8/scalars, controls,
/// unrelated format characters, punctuation-only lines and normalized key
/// echoes are dropped. Pure Han candidates still require a matching known
/// syllable count; mixed/English/digit/emoji results cannot use that
/// one-Han-per-syllable rule.
///
/// @param content raw model output (bounded by MAX_RESPONSE_BYTES)
/// @param pinyin request keys, used for pure-Han length and exact echo checks
/// @param maxCandidates upper bound on returned candidates, 0 means no limit
std::vector<std::string> sanitize(std::string_view content,
                                  std::string_view pinyin,
                                  size_t maxCandidates);

/// Count the syllables of a pinyin string as separated by ' or space.
///
/// Returns 0 when the string carries no separator at all, which means the
/// syllable boundary is unknown (full spelling without apostrophes, initials,
/// or a mixed abbreviation) and no length check can be performed.
size_t countSyllables(std::string_view pinyin);

/// Whether a string is only Han characters plus the CJK punctuation we allow.
bool isAllHan(std::string_view text);

/// Whether a string looks like plain pinyin keys, i.e. only a-z, ' and space.
///
/// The pinyin engine renders the partially selected preedit as a mix of Han
/// characters and pinyin ("你好 ni hao"). We only take over when nothing has
/// been selected yet, and this predicate is how that is decided: it is the one
/// property we can check exactly through the public API.
bool isPlainPinyin(std::string_view text);

} // namespace fcitx::chinesellm

#endif // _CHINESE_LLM_LLMBACKEND_H_
