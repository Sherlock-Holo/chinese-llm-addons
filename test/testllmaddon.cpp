/*
 * SPDX-FileCopyrightText: 2026-2026
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
// End to end test: a real fcitx5 instance, the real pinyin engine, the real
// addon, and a mock HTTP endpoint. It types a pinyin string through the test
// frontend and asserts that a candidate produced by the model shows up in the
// candidate list, then selects it and asserts the text is committed.
//
// The addon and data directories are passed through the environment by the
// caller (see scripts/smoke.sh), because fcitx5 reads them when it builds its
// StandardPaths, before this process constructs an Instance. Using
// setupTestingEnvironment() instead replaces the whole path table and makes the
// system pinyin addon's own dependencies unreachable.
#include "testfrontend_public.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/macros.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace fcitx;

namespace {

int failures = 0;
/// Set when the test finished, so the polling lambda only reports once.
bool finished = false;

void fail(const std::string &what) {
    std::cerr << "FAIL: " << what << "\n";
    failures++;
}

std::vector<std::string> candidateTexts(InputContext *ic) {
    std::vector<std::string> out;
    auto list = ic->inputPanel().candidateList();
    if (!list) {
        return out;
    }
    auto *bulk = list->toBulk();
    if (!bulk) {
        return out;
    }
    for (int i = 0; i < bulk->totalSize(); i++) {
        out.push_back(bulk->candidateFromAll(i).text().toString());
    }
    return out;
}

bool contains(const std::vector<std::string> &haystack,
              const std::string &needle) {
    return std::ranges::find(haystack, needle) != haystack.end();
}

void printCandidates(const std::string &label, InputContext *ic) {
    std::cout << label << ":";
    for (const auto &text : candidateTexts(ic)) {
        std::cout << " [" << text << "]";
    }
    std::cout << "\n";
}

} // namespace

int main(int argc, char **argv) {
    const std::string endpoint =
        argc > 1 ? argv[1] : "http://127.0.0.1:8899/v1/chat/completions";

    Log::setLogRule("default=3,chinese-llm=4");

    char arg0[] = "testllmaddon";
    char arg1[] = "--disable=all";
    // Repeated --enable options overwrite each other (the parser assigns rather
    // than appends), so every addon must be listed in one comma separated
    // argument.
    char arg2[] = "--enable=testfrontend,punctuation,pinyin,chinese-llm";
    char *argv2[] = {arg0, arg1, arg2};
    Instance instance(FCITX_ARRAY_SIZE(argv2), argv2);
    instance.addonManager().registerDefaultLoader(nullptr);

    EventDispatcher dispatcher;
    dispatcher.attach(&instance.eventLoop());

    int exitCode = 0;
    std::unique_ptr<EventSourceTime> poll;

    dispatcher.schedule([&]() {
        auto *testfrontend = instance.addonManager().addon("testfrontend", true);
        if (!testfrontend) {
            fail("testfrontend addon missing");
            exitCode = 1;
            instance.exit();
            return;
        }
        if (!instance.addonManager().addon("punctuation", true)) {
            fail("punctuation addon missing");
            exitCode = 1;
            instance.exit();
            return;
        }
        if (!instance.addonManager().addon("pinyin", true)) {
            fail("pinyin addon missing");
            exitCode = 1;
            instance.exit();
            return;
        }
        auto *llm = instance.addonManager().addon("chinese-llm", true);
        if (!llm) {
            fail("chinese-llm addon missing");
            exitCode = 1;
            instance.exit();
            return;
        }

        // Point the addon at the mock endpoint through its configuration, so
        // the test does not depend on the developer's real config file.
        RawConfig config;
        config.setValueByPath("Endpoint", endpoint);
        config.setValueByPath("Enabled", "True");
        // Debouncing is exercised by scripts/smoke.sh separately; here it is
        // disabled so the reply comes back at a predictable time.
        config.setValueByPath("DebounceMs", "0");
        config.setValueByPath("MinPinyinLength", "4");
        // The mock server reads the pinyin from the last line, so keep the
        // template in the shape it expects.
        config.setValueByPath("PromptTemplate", "{context}\n{pinyin}");
        config.setValueByPath("MaxCandidates", "3");
        config.setValueByPath("CandidateIndex", "2");
        llm->setConfig(config);
        std::cout << "addons loaded, endpoint " << endpoint << "\n";

        auto uuid =
            testfrontend->call<ITestFrontend::createInputContext>("testllmaddon");
        auto *ic = instance.inputContextManager().findByUUID(uuid);
        if (!ic) {
            fail("failed to create input context");
            exitCode = 1;
            instance.exit();
            return;
        }
        ic->focusIn();

        // The active input method comes from the group config; set it
        // explicitly so the pinyin engine handles the keys regardless of the
        // host machine's fcitx configuration.
        instance.setCurrentInputMethod("pinyin");
        if (instance.inputMethod(ic) != "pinyin") {
            fail("pinyin is not active, got: " + instance.inputMethod(ic));
            exitCode = 1;
            instance.exit();
            return;
        }
        std::cout << "active input method: " << instance.inputMethod(ic) << "\n";

        auto sendKey = [&](KeySym sym) {
            testfrontend->call<ITestFrontend::sendKeyEvent>(uuid, Key(sym),
                                                            false);
        };
        // "tamenzhengzaikaihui" -- the pinyin engine itself can assemble
        // 他们正在开会, so that phrase cannot prove the model ran. 她们正在开会
        // is never produced by the engine for this input (verified against the
        // candidates with the endpoint dead), so its presence is exclusive
        // evidence of the model path.
        for (const char c : std::string("tamenzhengzaikaihui")) {
            sendKey(KeySym(FcitxKey_a + (c - 'a')));
        }
        printCandidates("immediately after typing", ic);

        struct Context {
            Instance *instance;
            AddonInstance *frontend;
            InputContext *ic;
            ICUUID uuid;
            std::chrono::steady_clock::time_point deadline;
            int *exitCode;
        };
        static Context ctx;
        ctx.instance = &instance;
        ctx.frontend = testfrontend;
        ctx.ic = ic;
        ctx.uuid = uuid;
        ctx.deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(8);
        ctx.exitCode = &exitCode;

        poll = instance.eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 50000, 50000,
            [](EventSourceTime *, uint64_t) -> bool {
                if (finished) {
                    return true;
                }
                if (contains(candidateTexts(ctx.ic), "她们正在开会")) {
                    finished = true;
                    printCandidates("candidates with model output", ctx.ic);
                    std::cout << "ok: model candidate appeared\n";

                    // Selecting it must commit the text and clear the
                    // composition, which is the whole point of the reset in
                    // LlmCandidateWord::select.
                    auto texts = candidateTexts(ctx.ic);
                    const auto index = static_cast<int>(
                        std::ranges::find(texts, "她们正在开会") - texts.begin());
                    auto list = ctx.ic->inputPanel().candidateList();
                    list->candidate(index).select(ctx.ic);
                    const auto preedit =
                        ctx.ic->inputPanel().clientPreedit().toString();
                    if (!preedit.empty()) {
                        fail("preedit not cleared after selecting, got: " +
                             preedit);
                        *ctx.exitCode = 1;
                    } else {
                        std::cout << "ok: preedit cleared after commit\n";
                    }
                    ctx.frontend->call<ITestFrontend::destroyInputContext>(
                        ctx.uuid);
                    ctx.instance->exit();
                    return true;
                }
                if (std::chrono::steady_clock::now() > ctx.deadline) {
                    finished = true;
                    printCandidates("timed out with candidates", ctx.ic);
                    fail("model candidate 她们正在开会 did not appear within 8s");
                    *ctx.exitCode = 1;
                    ctx.frontend->call<ITestFrontend::destroyInputContext>(
                        ctx.uuid);
                    ctx.instance->exit();
                    return true;
                }
                return true;
            });
        poll->setOneShot();
    });

    instance.exec();
    return exitCode != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
