/*
 * SPDX-FileCopyrightText: 2026-2026
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "chinese-llm.h"
#include <fcitx-config/dbushelper.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/dbus/variant.h>
#include <iostream>
#include <map>
#include <sstream>

int main() {
    using fcitx::chinesellm::ChineseLlmConfig;
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
    }
    std::cout << "All config tests passed\n";
}
