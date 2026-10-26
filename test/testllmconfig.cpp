/*
 * SPDX-FileCopyrightText: 2026-2026
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "chinese-llm.h"
#include <algorithm>
#include <clocale>
#include <cstdlib>
#include <fcitx-config/dbushelper.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/dbus/variant.h>
#include <filesystem>
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
    fcitx::RawConfig raw;
    std::istringstream input(
        "Endpoint=http://127.0.0.1:8080/v1/chat/completions\n"
        "Model=beam-0.6b\n"
        "ApiKey=test-token\n"
        "Enabled=False\n"
        "ContextLength=2\n");
    fcitx::readFromIni(raw, input);
    config.load(raw, true);
    if (config.endpoint.value() !=
            "http://127.0.0.1:8080/v1/chat/completions" ||
        config.model.value() != "beam-0.6b" ||
        config.apiKey.value() != "test-token" || config.enabled.value() ||
        config.contextLength.value() != 2) {
        std::cerr << "flat INI configuration was not loaded\n";
        return 1;
    }
    fcitx::RawConfig saved;
    config.save(saved);
    std::ostringstream output;
    fcitx::writeAsIni(saved, output);
    if (output.str().find("[ChineseLlm]") != std::string::npos ||
        output.str().find("Model=beam-0.6b") == std::string::npos) {
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
        {"SystemPrompt",
         "请保持简短且明确。模型必须仅输出汉字，否则输出会被丢弃。"},
        {"PromptTemplate", "{context} 会替换为最近输入并确认的文本，{pinyin} "
                           "会替换为正在输入的拼音。"}};
    if (!root || root->subItemsSize() != expected.size()) {
        std::cerr << "root GUI description does not expose 18 independent "
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
            if (!tooltip || (chinese && *tooltip != translation)) {
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
             "候选词。停用后仍可使用原生拼音。"}};
        for (const auto &[english, translation] : statusTranslations) {
            if (fcitx::translateDomain("chinese-llm", english) !=
                (chinese ? translation : english)) {
                std::cerr << "wrong localized LLM status text\n";
                return 1;
            }
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
