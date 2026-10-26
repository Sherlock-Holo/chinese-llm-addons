#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate audited private variants; never modify the upstream source tree.

The input manifest pins every reviewed source. Any upstream drift fails closed
and requires updating both the transformations and the reviewed digest. Every
replacement checks its cardinality and is recorded in generation-audit.json.
--check validates all transformations in memory without writing or compiling.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

PINYIN_FILES = (
    "pinyin.cpp", "pinyin.h", "pinyincandidate.cpp", "pinyincandidate.h",
    "customphrase.cpp", "customphrase.h", "symboldictionary.cpp",
    "symboldictionary.h", "workerthread.cpp", "workerthread.h",
)
HELPER_FILES = {
    "punctuation": ("punctuation.cpp", "punctuation.h", "punctuation_public.h"),
    "fullwidth": ("fullwidth.cpp", "fullwidth.h"),
    "chttrans": ("chttrans.cpp", "chttrans.h", "chttrans-native.cpp", "chttrans-native.h"),
    "pinyinhelper": ("pinyinhelper.cpp", "pinyinhelper.h", "pinyinhelper_public.h",
                     "pinyinlookup.cpp", "pinyinlookup.h", "stroke.cpp", "stroke.h"),
}
# Defined in reviewed-sources.json, not inferred from whatever happens to be on
# disk at configure time. This is deliberately a migration/review boundary.
AUDIT = []


def replace(text, old, new, file, expected=1):
    count = text.count(old)
    if count != expected:
        raise ValueError(f"{file}: replacement {old!r}: expected {expected}, got {count}")
    AUDIT.append({"file": file, "old": old, "new": new, "count": count})
    return text.replace(old, new)


def replace_present(text, old, new, file):
    # Input SHA has already been verified; record the exact reviewed source's
    # cardinality for the audit. This helper is only for repetitive literal IDs.
    count = text.count(old)
    if not count:
        raise ValueError(f"{file}: required replacement absent: {old!r}")
    return replace(text, old, new, file, count)


def namespace(text, file):
    return replace(text, "namespace fcitx {", "namespace fcitx::llmnative {", file)


def global_namespace(text, start, file, header=False):
    text = replace(text, start, "namespace fcitx::llmnative {\n\n" + start, file)
    if header:
        end = text.rfind("#endif")
        if end < 0:
            raise ValueError(f"{file}: header guard missing")
        text = text[:end] + "} // namespace fcitx::llmnative\n\n" + text[end:]
    else:
        text += "\n} // namespace fcitx::llmnative\n"
    return text


def scope_gate(variable, instance="instance_"):
    return (f'{variable} && {instance}->inputMethod({variable}) == "llm-pinyin" && '
            f'{instance}->inputMethodEntry({variable}) && '
            f'{instance}->inputMethodEntry({variable})->addon() == "chinese-llm"')


def pinyin(text, file):
    if file.endswith(("workerthread.h", "workerthread.cpp")):
        start = "class TaskToken" if file.endswith(".h") else "WorkerThread::WorkerThread"
        text = global_namespace(text, start, file, file.endswith(".h"))
    else:
        text = namespace(text, file)
    # Preserve the cloud public-header relative layout at the shallower native
    # pinyin directory; it is a compile-only compatibility type, never loaded.
    if '../../modules/cloudpinyin/cloudpinyin_public.h' in text:
        text = replace(text, '../../modules/cloudpinyin/cloudpinyin_public.h',
                       '../modules/cloudpinyin/cloudpinyin_public.h', file)
    # Only quoted runtime paths; never rewrite libime/pinyin includes.
    for old, new in (
        ('"conf/pinyin.conf"', '"conf/chinese-llm-pinyin.conf"'),
        ('"pinyin/', '"chinese-llm/'),
        ('"pinyinState"', '"chinese-llm-pinyinState"'),
        ('"pinyin-prediction"', '"chinese-llm-pinyin-prediction"'),
        ('"fcitx-pinyin"', '"fcitx-chinese-llm-pinyin"'),
        ('"fcitx-cloudpinyin-toggle"', '"fcitx-chinese-llm-cloudpinyin-toggle"'),
        ('"fcitx://config/addon/pinyin/', '"fcitx://config/addon/chinese-llm/pinyin/'),
        ('"fcitx://config/addon/punctuation/', '"fcitx://config/addon/chinese-llm-punctuation/'),
        ('"fcitx://config/addon/chttrans"', '"fcitx://config/addon/chinese-llm-chttrans"'),
    ):
        if old in text:
            text = replace_present(text, old, new, file)
    if file.endswith("pinyin.h"):
        # These are Fcitx core types, not implementation classes. Moving their
        # forward declarations would shadow core and break pointer conversions.
        text = replace(text, 'struct EventSourceTime;', 'using EventSourceTime = ::fcitx::EventSourceTime;', file)
        text = replace(text, 'class CandidateList;', 'using CandidateList = ::fcitx::CandidateList;', file)
        text = replace(text, 'namespace fcitx::llmnative {',
            'namespace fcitx { class CandidateList; struct EventSourceTime; }\n\nnamespace fcitx::llmnative {', file)
        for addon in ("cloudpinyin", "quickphrase", "spell", "imeapi"):
            text = replace(text,
                f"FCITX_ADDON_DEPENDENCY_LOADER({addon}, instance_->addonManager());",
                f"AddonInstance *{addon}() const {{ return nullptr; }}", file)
        for addon in ("pinyinhelper", "fullwidth", "chttrans", "punctuation"):
            text = replace(text,
                f"FCITX_ADDON_DEPENDENCY_LOADER({addon}, instance_->addonManager());",
                f'AddonInstance *{addon}() {{ return instance_->addonManager().addon("chinese-llm-{addon}", false); }}', file)
        text = replace(text,
            "FCITX_ADDON_DEPENDENCY_LOADER(notifications, instance_->addonManager());",
            'AddonInstance *notifications() { return instance_->addonManager().addon("notifications", false); }', file)
        # All unsupported integrations are hidden AND forcibly disabled after
        # load: supplying old raw keys cannot reconnect a shared mutable addon.
        for old, new in (
            ("Option<bool> spellEnabled", "HiddenOption<bool> spellEnabled"),
            ('_("Show English Candidates"), true', '_("Show English Candidates"), false'),
            ("ExternalOption dictmanager", "HiddenOption<std::string> dictmanager"),
            ("ExternalOption customphrase", "HiddenOption<std::string> customphrase"),
            ("Option<Key, KeyConstrain> quickphraseKey", "HiddenOption<Key> quickphraseKey"),
            ("Option<bool> useVAsQuickphrase", "HiddenOption<bool> useVAsQuickphrase"),
            ("ExternalOption quickphrase", "HiddenOption<std::string> quickphrase"),
            ("Option<std::vector<std::string>, ListConstrain<RegexConstrain>,\n           DefaultMarshaller<std::vector<std::string>>, ToolTipAnnotation>\n        quickphraseTriggerRegex",
             "HiddenOption<std::vector<std::string>> quickphraseTriggerRegex"),
        ):
            text = replace(text, old, new, file)
        # HiddenOption has no constrain/annotation parameters; replace the two
        # declarations as whole blocks rather than creating invalid templates.
        # Preserve the original formatting-independent blocks with bounded
        # anchors; SHA pinning and replacement cardinality guard the boundary.
        start = text.index("    HiddenOption<Key> quickphraseKey{")
        end = text.index("    HiddenOption<bool> useVAsQuickphrase", start)
        text = replace(text, text[start:end],
            '    HiddenOption<Key> quickphraseKey{this, "QuickPhraseKey", "QuickPhraseKey", Key{}};\n', file)
        start = text.index("    HiddenOption<std::vector<std::string>> quickphraseTriggerRegex{")
        end = text.index("    Option<FuzzyConfig> fuzzyConfig", start)
        text = replace(text, text[start:end],
            '    HiddenOption<std::vector<std::string>> quickphraseTriggerRegex{this, "QuickPhraseTriggerRegex", "QuickPhraseTriggerRegex", {}};\n', file)
    elif file.endswith("pinyin.cpp"):
        text = replace(text, 'FCITX_DEFINE_LOG_CATEGORY(pinyin, "pinyin");',
                       'FCITX_DEFINE_LOG_CATEGORY(pinyin, "chinese-llm-pinyin");', file)
        text = replace(text, 'entry->addon() != "pinyin"',
                       'entry->addon() != "chinese-llm" || entry->uniqueName() != "llm-pinyin"', file)
        text = replace(text, 'entry.uniqueName() == "pinyin"',
                       'entry.uniqueName() == "llm-pinyin"', file)
        text = replace(text, 'state->context_.setUseShuangpin(entry.uniqueName() == "shuangpin");',
                       'state->context_.setUseShuangpin(false);', file)
        text = replace(text,
            'for (const auto *actionName : {"chttrans", "punctuation", "fullwidth"})',
            'for (const auto *actionName : {"chinese-llm-chttrans", "chinese-llm-punctuation", "chinese-llm-fullwidth"})', file)
        text = replace(text, "void PinyinEngine::populateConfig() {",
            "void PinyinEngine::populateConfig() {\n"
            "    config_.spellEnabled.setValue(false);\n"
            "    config_.cloudPinyinEnabled.setValue(false);\n"
            "    config_.useVAsQuickphrase.setValue(false);\n"
            "    config_.quickphraseKey.setValue(Key{});\n"
            "    config_.quickphraseTriggerRegex.setValue(std::vector<std::string>{});\n"
            "    config_.firstRun.setValue(false);\n"
            "    config_.cloudPinyinEnabled.annotation().setHidden(true);\n"
            "    config_.cloudPinyinIndex.annotation().setHidden(true);\n"
            "    config_.cloudPinyinAnimation.annotation().setHidden(true);\n"
            "    config_.keepCloudPinyinPlaceHolder.annotation().setHidden(true);\n"
            "    config_.cloudpinyin.setHidden(true);\n"
            "    config_.shuangpinProfile.annotation().setHidden(true);\n"
            "    config_.showShuangpinMode.annotation().setHidden(true);\n"
            "    config_.fuzzyConfig->partialSp.annotation().setHidden(true);", file)
        # Disable the legacy branch's standalone IM identity even in subMode.
        text = replace(text, 'entry.uniqueName() == "shuangpin"', 'false', file)
        text = replace(text,
            'fcitx::InputContextEvent &event) {\n    auto *inputContext = event.inputContext();\n    // Request addons',
            'fcitx::InputContextEvent &event) {\n    FCITX_UNUSED(entry);\n    auto *inputContext = event.inputContext();\n    // Request addons', file)
        text = replace(text, '    FCITX_UNUSED(inputContext);\n    if (false &&',
            '    FCITX_UNUSED(inputContext);\n    FCITX_UNUSED(entry);\n    if (false &&', file)
        # LibIME's resolver uses global user data search. Bypass it deliberately:
        # only its installation-provided model and dictionaries may be shared.
        text = replace(text,
            'libime::DefaultLanguageModelResolver::instance()\n                .languageModelFileForLanguage("zh_CN")',
            'CHINESE_LLM_SYSTEM_LANGUAGE_MODEL', file)
        text = replace(text,
            'standardPath.open(StandardPathsType::Data, "libime/sc.dict")',
            'standardPath.open(StandardPathsType::Data, LIBIME_INSTALL_PKGDATADIR "/sc.dict", StandardPathsMode::System)', file)
        text = replace(text,
            'standardPath.locate(StandardPathsType::Data, "libime/extb.dict")',
            'standardPath.locate(StandardPathsType::Data, LIBIME_INSTALL_PKGDATADIR "/extb.dict", StandardPathsMode::System)', file)
        text = replace(text,
            'LIBIME_INSTALL_PKGDATADIR "/extb.dict");',
            'LIBIME_INSTALL_PKGDATADIR "/extb.dict", StandardPathsMode::System);', file)
        text = replace(text, "[this](InputContext *ic) {\n            config_.predictionEnabled",
            '[this](InputContext *ic) {\n            if (!(' + scope_gate("ic") + ')) { return; }\n            config_.predictionEnabled', file)
    # Runtime names are isolated; shuangpin branch in subMode is unreachable
    # because only the owning wrapper supplies this private engine's entry.
    forbidden = ('"conf/pinyin.conf"', '"pinyin/', '"pinyinState"',
                 '"pinyin-prediction"', 'addon("pinyin"',
                 'FCITX_ADDON_DEPENDENCY_LOADER(punctuation,',
                 'FCITX_ADDON_DEPENDENCY_LOADER(pinyinhelper,')
    if any(value in text for value in forbidden):
        raise ValueError(f"{file}: original mutable runtime identity remains")
    return text


def helper(text, module, file):
    if file.endswith("_public.h"):
        # Interface metadata uses fcitx::IPunctuation/IPinyinHelper; preserving
        # those signatures does not instantiate or load the official addon.
        return text
    if module == "pinyinhelper":
        text = namespace(text, file)
    elif file.endswith(".h"):
        starts = {"punctuation.h": "FCITX_CONFIGURATION(\n    PunctuationConfig",
                  "fullwidth.h": "FCITX_CONFIGURATION(",
                  "chttrans.h": "#ifdef ENABLE_OPENCC\nstruct OpenCCAnnotation",
                  "chttrans-native.h": "class NativeBackend"}
        text = global_namespace(text, starts[Path(file).name], file, True)
    else:
        text = global_namespace(text, "using namespace fcitx;", file)
    if module == "punctuation" and file.endswith("punctuation.cpp"):
        # Data directories and UI action/addon IDs are intentionally different.
        # Do this bounded directory replacement before the generic action ID.
        text = replace(text, '"punctuation", filter', '"chinese-llm/punctuation", filter', file, 2)
    for old, new in (
        (f'"conf/{module}.conf"', f'"conf/chinese-llm-{module}.conf"'),
        (f'"{module}/', f'"chinese-llm/{module}/'),
        ('"punctuationState"', '"chinese-llm-punctuationState"'),
        ('"punctuation"', '"chinese-llm-punctuation"'),
        ('"fullwidth"', '"chinese-llm-fullwidth"'),
        ('"chttrans"', '"chinese-llm-chttrans"'),
        ('"fcitx-punc-toggle"', '"fcitx-chinese-llm-punc-toggle"'),
        ('"fcitx-fullwidth-toggle"', '"fcitx-chinese-llm-fullwidth-toggle"'),
        ('"fcitx-chttrans-toggle"', '"fcitx-chinese-llm-chttrans-toggle"'),
    ):
        if old in text:
            text = replace_present(text, old, new, file)
    if file.endswith(".cpp"):
        factories = {"punctuation": "PunctuationFactory", "fullwidth": "FullwidthModuleFactory",
                     "chttrans": "ChttransModuleFactory", "pinyinhelper": "PinyinHelperModuleFactory"}
        factory = factories[module]
        old = f"FCITX_ADDON_FACTORY_V2({module}, " + ("fcitx::" if module == "pinyinhelper" else "") + factory + ")"
        if old in text:
            text = replace(text, old, f"FCITX_ADDON_FACTORY(fcitx::llmnative::{factory})", file)
    if module in ("punctuation", "fullwidth") and file.endswith(".h"):
        text = replace(text, 'void setEnabled(bool enabled, fcitx::InputContext *ic) {',
            'void setEnabled(bool enabled, fcitx::InputContext *ic) {\n        if (!inWhiteList(ic)) { return; }', file)
    if module == "punctuation" and file.endswith("punctuation.cpp"):
        text = replace(text,
            'return toggleAction_.isParent(&inputContext->statusArea());',
            'return (' + scope_gate("inputContext") + ') && toggleAction_.isParent(&inputContext->statusArea());', file)
        text = replace(text,
            '[this](InputContext *ic, const std::string &sentence) {',
            '[this](InputContext *ic, const std::string &sentence) {\n            if (!inWhiteList(ic)) { return; }', file)
        text = replace(text,
            'auto processKeyEvent = [this](const KeyEventBase &keyEvent) {',
            'auto processKeyEvent = [this](const KeyEventBase &keyEvent) {\n        if (!inWhiteList(keyEvent.inputContext())) { return; }', file)
        # Focus/reset/surrounding observers must not even update private state
        # for another IM, unlike the upstream global punctuation observer.
        text = replace_present(text, 'auto *ic = icEvent.inputContext();',
            'auto *ic = icEvent.inputContext();\n            if (!inWhiteList(ic)) { return; }', file)
    if module == "fullwidth" and file.endswith("fullwidth.cpp"):
        text = replace(text,
            'return toggleAction_.isParent(&inputContext->statusArea());',
            'return (' + scope_gate("inputContext") + ') && toggleAction_.isParent(&inputContext->statusArea());', file)
    if module == "chttrans" and file.endswith("chttrans.cpp"):
        for variable in ("ic", "inputContext"):
            old = f'toggleAction_.isParent(&{variable}->statusArea())'
            text = replace_present(text, old, '(' + scope_gate(variable) + ' && ' + old + ')', file)
        # Exact scoped predicate, not only status action ownership, also applies
        # to the query functions called by the status action.
        text = replace(text,
            'if (!engine || !entry) {',
            'if (!engine || !entry || entry->uniqueName() != "llm-pinyin" || entry->addon() != "chinese-llm") {', file)
    if module == "pinyinhelper" and file.endswith("pinyinhelper.h"):
        text = replace(text, '#include <quickphrase_public.h>\n', '', file)
        for addon in ("quickphrase", "clipboard"):
            text = replace(text,
                f"    FCITX_ADDON_DEPENDENCY_LOADER({addon}, instance_->addonManager());\n", '', file)
        text = replace(text, '    void initQuickPhrase();\n', '', file)
        text = replace(text, '    std::unique_ptr<EventSource> deferEvent_;\n', '', file)
        text = replace(text, '    std::unique_ptr<HandlerTableEntry<QuickPhraseProviderCallback>> handler_;\n', '', file)
    if module == "pinyinhelper" and file.endswith("pinyinhelper.cpp"):
        text = replace(text, '#include <clipboard_public.h>\n', '', file)
        start = text.index('PinyinHelper::PinyinHelper(Instance *instance)')
        end = text.index('std::vector<std::string> PinyinHelper::lookup(uint32_t chr)', start)
        text = replace(text, text[start:end],
            'PinyinHelper::PinyinHelper(Instance *instance) : instance_(instance) {}\n\n', file)
    return text


def generate(root, manifest):
    outputs = {}
    sources = json.loads(manifest.read_text(encoding="utf-8"))
    for relative, expected in sources.items():
        source = root / relative
        if not source.is_file() or source.is_symlink():
            raise ValueError(f"not a regular source: {relative}")
        content = source.read_bytes()
        actual = hashlib.sha256(content).hexdigest()
        if actual != expected:
            raise ValueError(f"unreviewed upstream source drift: {relative}: {actual} != {expected}")
        AUDIT.append({"source": relative, "sha256": actual})
        text = content.decode("utf-8")
        if relative.startswith("im/pinyin/"):
            outputs['pinyin/' + source.name] = pinyin(text, relative)
        elif relative == "modules/cloudpinyin/cloudpinyin_public.h":
            # Keep ../../modules/... relative include layout used by pinyin.
            outputs['modules/cloudpinyin/cloudpinyin_public.h'] = text
        else:
            module = source.parent.name
            outputs['modules/' + module + '/' + source.name] = helper(text, module, relative)
    expected_paths = {'im/pinyin/' + f for f in PINYIN_FILES}
    expected_paths.add('modules/cloudpinyin/cloudpinyin_public.h')
    for module, files in HELPER_FILES.items():
        expected_paths.update('modules/' + module + '/' + f for f in files)
    if set(sources) != expected_paths:
        raise ValueError("reviewed source manifest and generator file allowlist disagree")
    outputs['generation-audit.json'] = json.dumps(AUDIT, ensure_ascii=False, indent=2) + "\n"
    return outputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    outputs = generate(args.source.resolve(), Path(__file__).with_name('reviewed-sources.json'))
    if not args.check:
        if args.output is None:
            parser.error('--output is required without --check')
        output = args.output.resolve()
        source = args.source.resolve()
        protected = (source / 'im', source / 'modules', source / 'cmake', source / 'test', source / 'scripts')
        if output == source or any(output == path or path in output.parents for path in protected):
            raise ValueError('generation output must be a build directory, never an upstream source directory')
        # Validation completed before writing any output, so a failed migration
        # cannot leave a partially updated build-tree source set.
        for relative, content in outputs.items():
            path = output / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding='utf-8')
    print(f'validated {len(outputs) - 1} private generated files; {len(AUDIT)} audit entries')


if __name__ == '__main__':
    main()
