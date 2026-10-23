/*
 * SPDX-FileCopyrightText: 2026-2026
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "chinese-llm.h"
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <iostream>
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
    std::cout << "All config tests passed\n";
}
