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
/// not a string, or when the reply carries no usable text.
std::string parseResponse(std::string_view body);

/// Turn raw model output into candidates that are safe to commit.
///
/// This is the only place that decides whether model output may reach the
/// candidate list, and it is deliberately strict: anything it cannot verify is
/// dropped rather than shown. Each rule below exists because of an observed
/// failure mode.
///
/// @param content raw model output
/// @param pinyin the pinyin the request was made for, used for length checks
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
