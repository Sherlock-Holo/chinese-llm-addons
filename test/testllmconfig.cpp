/*
 * SPDX-FileCopyrightText: 2026-2026
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "chinese-llm.h"
#include <algorithm>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <fcitx-config/dbushelper.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/dbus/variant.h>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <sstream>

int main(int argc, char *argv[]) {
    using fcitx::chinesellm::ChineseLlmConfig;
    const bool localized = argc == 3;
    const bool chinese = localized && std::string(argv[1]) == "zh_CN";
    if (argc != 1 &&
        (!localized || (!chinese && std::string(argv[1]) != "en"))) {
        std::cerr << "usage: testllmconfig [zh_CN|en locale-directory]\n";
        return 1;
    }
    if (localized) {
        // Separate CTest processes avoid gettext's first-use domain cache.
        if (chinese) {
            setenv("LANGUAGE", "zh_CN", 1);
            if (!std::setlocale(LC_ALL, "zh_CN.UTF-8") &&
                !std::setlocale(LC_ALL, "en_US.UTF-8")) {
                std::cerr << "Chinese translation test requires zh_CN.UTF-8 or "
                             "en_US.UTF-8; coverage skipped\n";
                return 77;
            }
        } else {
            setenv("LANGUAGE", "en", 1);
            std::setlocale(LC_ALL, "C");
        }
        fcitx::registerDomain("chinese-llm", std::filesystem::path{argv[2]});
    }
    ChineseLlmConfig config;
    if (!config.recordHistory.value()) {
        std::cerr << "request history must be enabled by default\n";
        return 1;
    }
    const auto &defaultPrompt = config.systemPrompt.value();
    if (defaultPrompt.find("Preserve English words, numbers and emoji") ==
            std::string::npos ||
        defaultPrompt.find("Output only candidate text on a single line") ==
            std::string::npos ||
        defaultPrompt.find("No echoed keys") == std::string::npos ||
        defaultPrompt.find("Han characters only") != std::string::npos) {
        std::cerr << "default prompt must support mixed-language candidates "
                     "without permitting key echo/explanations\n";
        return 1;
    }
    fcitx::RawConfig raw;
    std::istringstream input(
        "Endpoint=http://127.0.0.1:8080/v1/chat/completions\n"
        "Model=beam-0.6b\n"
        "ApiKey=test-token\n"
        "Enabled=False\n"
        "RecordHistory=False\n"
        "ContextLength=2\n");
    fcitx::readFromIni(raw, input);
    config.load(raw, true);
    if (config.endpoint.value() !=
            "http://127.0.0.1:8080/v1/chat/completions" ||
        config.model.value() != "beam-0.6b" ||
        config.apiKey.value() != "test-token" || config.enabled.value() ||
        config.recordHistory.value() || config.contextLength.value() != 2) {
        std::cerr << "flat INI configuration was not loaded\n";
        return 1;
    }
    fcitx::RawConfig saved;
    config.save(saved);
    std::ostringstream output;
    fcitx::writeAsIni(saved, output);
    if (output.str().find("[ChineseLlm]") != std::string::npos ||
        output.str().find("Model=beam-0.6b") == std::string::npos ||
        output.str().find("RecordHistory=False") == std::string::npos) {
        std::cerr << "configuration did not round-trip as flat INI\n";
        return 1;
    }
    fcitx::RawConfig description;
    config.dumpDescription(description);
    const std::string rootType = config.typeName();
    const auto root = description.get(rootType);
    const std::map<std::string, std::string> expected{
        {"NativePinyin", "External"},
        {"Enabled", "Boolean"},
        {"Endpoint", "String"},
        {"Model", "String"},
        {"ApiKey", "String"},
        {"MinPinyinLength", "Integer"},
        {"DebounceMs", "Integer"},
        {"TimeoutSeconds", "Integer"},
        {"MaxCandidates", "Integer"},
        {"CandidateIndex", "Integer"},
        {"ContextLength", "Integer"},
        {"CacheSize", "Integer"},
        {"MaxConsecutiveErrors", "Integer"},
        {"CooldownSeconds", "Integer"},
        {"ShowBadge", "Boolean"},
        {"RecordHistory", "Boolean"},
        {"ToggleKey", "List|Key"},
        {"SystemPrompt", "String"},
        {"PromptTemplate", "String"}};
    const std::map<std::string, std::pair<std::string, std::string>> labels{
        {"NativePinyin", {"Independent Pinyin Settings", "独立拼音设置"}},
        {"Enabled", {"Enable LLM candidates", "启用 LLM 候选词"}},
        {"Endpoint", {"Endpoint", "接口地址"}},
        {"Model", {"Model", "模型"}},
        {"ApiKey", {"API Key", "API 密钥"}},
        {"MinPinyinLength", {"Minimum Pinyin Length", "最小拼音长度"}},
        {"DebounceMs", {"Debounce Delay (ms)", "输入防抖延迟（毫秒）"}},
        {"TimeoutSeconds", {"Request Timeout (seconds)", "请求超时时间（秒）"}},
        {"MaxCandidates", {"Max Candidates", "最大候选词数量"}},
        {"CandidateIndex", {"Candidate Position", "候选词插入位置"}},
        {"ContextLength",
         {"Context Length (characters)", "上下文长度（字符）"}},
        {"CacheSize", {"Cache Size", "缓存条目数"}},
        {"MaxConsecutiveErrors",
         {"Errors Before Cooldown", "进入冷却前的连续错误次数"}},
        {"CooldownSeconds", {"Cooldown (seconds)", "冷却时间（秒）"}},
        {"ShowBadge",
         {"Show cloud marker on LLM candidates",
          "在 LLM 候选词旁显示云朵标记"}},
        {"RecordHistory", {"Record request history", "记录请求历史"}},
        {"ToggleKey", {"Toggle Key", "启用／停用快捷键"}},
        {"SystemPrompt", {"System Prompt", "系统提示词"}},
        {"PromptTemplate", {"Prompt Template", "提示词模板"}}};
    const std::map<std::string, std::string> tooltips{
        {"ShowBadge", "在 LLM "
                      "生成的候选词旁显示云朵符号，符号不会输入到正文。词库候选"
                      "词不会标记，包括与模型结果重复的词库候选词。"},
        {"Endpoint", "兼容 OpenAI 的聊天补全接口地址，支持 Ollama、llama.cpp "
                     "的 llama-server 以及兼容的在线 API。"},
        {"ApiKey", "以 Bearer "
                   "令牌发送。不需要身份验证的本地服务可以留空；留空时会尝试使"
                   "用环境变量 FCITX_CHINESE_LLM_API_KEY。"},
        {"RecordHistory",
         "在内存中保留最近 100 条请求，不记录上下文或凭据。查看历史时创建私有"
         "临时快照；停用会清空历史。"},
        {"SystemPrompt",
         "请保持简短且明确。仅在一行中输出候选文本，不要解释或回显输入按键。"
         "支持中文、英文、数字和 emoji。"},
        {"PromptTemplate", "{context} 会替换为最近输入并确认的文本，{pinyin} "
                           "会替换为正在输入的拼音。"}};
    if (!root || root->subItemsSize() != expected.size()) {
        std::cerr << "root GUI description does not expose 19 independent "
                     "settings/link fields\n";
        return 1;
    }
    for (const auto &[field, type] : expected) {
        const auto *value =
            description.valueByPath(rootType + '/' + field + "/Type");
        const auto *label =
            description.valueByPath(rootType + '/' + field + "/Description");
        if (!value || *value != type || !label || label->empty()) {
            std::cerr << "missing/wrong GUI field/type/label: " << field
                      << '\n';
            return 1;
        }
    }
    if (localized) {
        for (const auto &[field, translations] : labels) {
            const auto *label = description.valueByPath(rootType + '/' + field +
                                                        "/Description");
            if (!label || *label != (chinese ? translations.second
                                             : translations.first)) {
                std::cerr << "wrong localized GUI label: " << field << '\n';
                return 1;
            }
        }
        for (const auto &[field, translation] : tooltips) {
            const auto *tooltip =
                description.valueByPath(rootType + '/' + field + "/Tooltip");
            if (!tooltip || (chinese && *tooltip != translation) ||
                (!chinese && field == "SystemPrompt" &&
                 *tooltip != "Keep this short and unambiguous. Output only "
                             "candidate text on a single line, without "
                             "explanations or echoed keys. Chinese, English, "
                             "digits and emoji are supported.") ||
                (!chinese && field == "RecordHistory" &&
                 *tooltip != "Keep the last 100 requests in memory, without "
                             "context or credentials. Viewing history creates "
                             "a private temporary snapshot. Disable to clear "
                             "history.")) {
                std::cerr << "wrong localized GUI tooltip: " << field << '\n';
                return 1;
            }
        }
        const std::map<std::string, std::string> statusTranslations{
            {"LLM candidates enabled", "LLM 候选词已启用"},
            {"LLM candidates disabled", "LLM 候选词已停用"},
            {"Click or use the toggle shortcut to enable or disable LLM "
             "candidates. Native pinyin remains available when disabled.",
             "点击此项或使用启用／停用快捷键切换 LLM "
             "候选词。停用后仍可使用原生拼音。"},
            {"LLM requests {0} (valid {1})", "LLM 请求 {0}（有效 {1}）"},
            {"LLM overlapping requests {0}", "LLM 重叠请求 {0}"},
            {"LLM overlap rate {0}", "LLM 重叠率 {0}"},
            {"LLM request history {0}", "LLM 请求历史 {0}"},
            {"Requests started in this Fcitx session, including failures and "
             "cancellations. Valid requests returned usable candidates for "
             "the current input. Cache hits are not requests.",
             "本次 Fcitx 运行发起的请求，包括失败和取消。有效请求指返回可用候"
             "选且仍对应当前输入的请求。缓存命中不算请求。"},
            {"Valid requests with at least one candidate identical to any "
             "native candidate. Each request is counted once, before applying "
             "the candidate display limit.",
             "有效请求中至少一个候选与任意原生候选完全相同的请求数。每个请"
             "求只计一次，不受候选显示数量限制。"},
            {"Overlapping requests divided by valid requests. No rate is shown "
             "until a valid request completes. Statistics reset when Fcitx "
             "restarts; no input text is saved.",
             "重叠请求数除以有效请求数。没有有效请求时不显示比例。重启 "
             "Fcitx 后统计清零，不保存输入文本。"}};
        for (const auto &[english, translation] : statusTranslations) {
            if (fcitx::translateDomain("chinese-llm", english) !=
                (chinese ? translation : english)) {
                std::cerr << "wrong localized LLM status text: " << english
                          << '\n';
                return 1;
            }
        }
        // Format translated short text, not just the untranslated placeholders.
        // All statistics must remain readable on panels that omit tooltips.
        try {
            const auto checkFormat = [&](const char *pattern,
                                         const std::string &expectedText,
                                         const auto &...args) {
                const auto actual =
                    std::vformat(fcitx::translateDomain("chinese-llm", pattern),
                                 std::make_format_args(args...));
                if (actual != expectedText) {
                    std::cerr << "wrong formatted LLM status text: " << pattern
                              << " -> " << actual << '\n';
                    return false;
                }
                return true;
            };
            const std::uint64_t sent = 17;
            const std::uint64_t compared = 3;
            const std::uint64_t overlapping = 1;
            const std::uint64_t zero = 0;
            const std::string noRate = "—";
            const auto rate = std::format("{:.1f}%", 100.0 / 3.0);
            const auto zeroRate = std::format("{:.1f}%", 0.0);
            const auto fullRate = std::format("{:.1f}%", 100.0);
            if (!checkFormat("LLM request history {0}",
                             chinese ? "LLM 请求历史 17"
                                     : "LLM request history 17",
                             sent) ||
                !checkFormat("LLM requests {0} (valid {1})",
                             chinese ? "LLM 请求 17（有效 3）"
                                     : "LLM requests 17 (valid 3)",
                             sent, compared) ||
                !checkFormat("LLM requests {0} (valid {1})",
                             chinese ? "LLM 请求 0（有效 0）"
                                     : "LLM requests 0 (valid 0)",
                             zero, zero) ||
                !checkFormat("LLM overlapping requests {0}",
                             chinese ? "LLM 重叠请求 1"
                                     : "LLM overlapping requests 1",
                             overlapping) ||
                !checkFormat("LLM overlapping requests {0}",
                             chinese ? "LLM 重叠请求 0"
                                     : "LLM overlapping requests 0",
                             zero) ||
                !checkFormat("LLM overlap rate {0}",
                             chinese ? "LLM 重叠率 —" : "LLM overlap rate —",
                             noRate) ||
                !checkFormat("LLM overlap rate {0}",
                             chinese ? "LLM 重叠率 33.3%"
                                     : "LLM overlap rate 33.3%",
                             rate) ||
                !checkFormat("LLM overlap rate {0}",
                             chinese ? "LLM 重叠率 0.0%"
                                     : "LLM overlap rate 0.0%",
                             zeroRate) ||
                !checkFormat("LLM overlap rate {0}",
                             chinese ? "LLM 重叠率 100.0%"
                                     : "LLM overlap rate 100.0%",
                             fullRate)) {
                return 1;
            }
        } catch (const std::format_error &error) {
            std::cerr << "invalid localized LLM status placeholders: "
                      << error.what() << '\n';
            return 1;
        }
        // The private catalog must also retain native pinyin/helper labels.
        if (std::string(
                fcitx::translateDomain("chinese-llm", "Candidates Per Page")) !=
                (chinese ? "每页候选词" : "Candidates Per Page") ||
            std::string(fcitx::translateDomain("chinese-llm",
                                               "Full width Character")) !=
                (chinese ? "全角字符" : "Full width Character") ||
            std::string(fcitx::translateDomain(
                "chinese-llm", "missing translation sentinel")) !=
                "missing translation sentinel") {
            std::cerr
                << "private native labels or English fallback are missing\n";
            return 1;
        }
    }
    const auto *uri =
        description.valueByPath(rootType + "/NativePinyin/External");
    const auto *launch =
        description.valueByPath(rootType + "/NativePinyin/LaunchSubConfig");
    if (!uri || *uri != "fcitx://config/addon/chinese-llm/pinyin" || !launch ||
        *launch != "True" || saved.get("NativePinyin")) {
        std::cerr << "native pinyin settings link is missing/shared or "
                     "serialized as user config\n";
        return 1;
    }
    const auto dbus = fcitx::dumpDBusConfigDescription(config);
    if (dbus.size() != 1 || std::get<0>(dbus.front()) != rootType ||
        std::get<1>(dbus.front()).size() != expected.size()) {
        std::cerr << "D-Bus GUI description lost root fields\n";
        return 1;
    }
    for (const auto &option : std::get<1>(dbus.front())) {
        const auto it = expected.find(std::get<0>(option));
        if (it == expected.end() || it->second != std::get<1>(option) ||
            std::get<2>(option).empty()) {
            std::cerr << "D-Bus GUI field type/label mismatch\n";
            return 1;
        }
        const auto &field = std::get<0>(option);
        if (localized &&
            std::get<2>(option) !=
                (chinese ? labels.at(field).second : labels.at(field).first)) {
            std::cerr << "D-Bus GUI lost localized label: " << field << '\n';
            return 1;
        }
        if (chinese && tooltips.count(field)) {
            const auto &extras = std::get<4>(option);
            const auto tooltip = std::find_if(
                extras.begin(), extras.end(),
                [](const auto &entry) { return entry.key() == "Tooltip"; });
            if (tooltip == extras.end() ||
                tooltip->value().dataAs<std::string>() != tooltips.at(field)) {
                std::cerr << "D-Bus GUI lost localized tooltip: " << field
                          << '\n';
                return 1;
            }
        }
    }
    std::cout << "All config tests passed\n";
}
