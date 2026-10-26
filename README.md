Chinese related addons for Fcitx5
================================
Upstream provides pinyin and table input methods, released under LGPL-2.1-or-later.
This fork preserves upstream source attribution. `im/pinyin/emoji.txt` is derived
from Unicode CLDR with modification.

# LLM拼音: independent native engine and LLM candidates

The `feature/chinese-llm` branch provides **LLM拼音**, with input-method id
`llm-pinyin` and addon id `chinese-llm`. It reuses upstream pinyin **source code**,
not the running distribution `pinyin` addon. A privately compiled native engine
owns its dictionaries, learning model, composition properties, prediction action
and native configuration. Model suggestions come from an OpenAI-compatible Chat
Completions endpoint and are inserted alongside its own dictionary candidates.

The official **拼音** (`pinyin`) and **双拼** (`shuangpin`) remain available and
unchanged. They never request the LLM or intercept the LLM toggle key. LLM拼音
continues to work if the official pinyin addon is absent or disabled.

## Isolation contract

These are separate, with no automatic import, fallback or migration from the
original pinyin's user files:

- Native engine/LibIME objects, user dictionary, learning history and prediction.
- Native settings, including fuzzy pinyin, candidate/page keys and learning.
- Custom phrases, extra dictionaries and their `.disable` markers.
- Punctuation profiles/state, fullwidth state and simplified/traditional
  conversion settings/actions, implemented by private helper addons.
- LLM endpoint, API key, prompts, cache and request lifecycle.

Typical paths, subject to the usual Fcitx/XDG path overrides:

```text
~/.config/fcitx5/conf/chinese-llm.conf             # LLM settings
~/.config/fcitx5/conf/chinese-llm-pinyin.conf      # native pinyin settings
~/.config/fcitx5/conf/chinese-llm-punctuation.conf
~/.config/fcitx5/conf/chinese-llm-fullwidth.conf
~/.config/fcitx5/conf/chinese-llm-chttrans.conf
~/.local/share/fcitx5/chinese-llm/user.dict
~/.local/share/fcitx5/chinese-llm/user.history
~/.local/share/fcitx5/chinese-llm/customphrase
~/.local/share/fcitx5/chinese-llm/dictionaries/
```

No code changes or installs replace the distribution's `libpinyin.so`,
`addon/pinyin.conf`, `inputmethod/pinyin.conf`, configuration or learning data.
Uninstalling this package does not uninstall or erase the original pinyin.

**Boundary:** both are still input methods in the same Fcitx process. Fcitx global
hotkeys, frontends, theme, desktop clipboard and global output filters are shared
infrastructure, not independently cloned desktop services. Global installed
addons may observe committed text. This is native input-method settings/data
isolation, not OS/process sandboxing. Read-only system LibIME dictionary/static
language-model files may be shared, but each native engine owns its mutable
in-memory dictionary/history. Original pinyin user overlays are not read.

**Compatibility:** ordinary dictionary input, prediction, native key bindings,
stroke lookup, punctuation, fullwidth and native simplified/traditional conversion
are provided privately. This build does not reuse the official cloudpinyin,
quickphrase, spell-provider or Lua runtime. Their unsupported options/editors are
hidden/disabled rather than pointing at the official configuration. In particular,
the original dictionary/custom-phrase GUI editors must not be used for LLM拼音's
files. Custom phrases/extra dictionaries can be managed in the private paths
above. Conversion currently uses the bundled native table, not shared OpenCC
profiles. Third-party plugins bound to exact `pinyin`/`shuangpin` names may not
recognize `llm-pinyin`.

## Build and safely package

The independent build requires a C++20 compiler, CMake, Python3, pkg-config,
Fcitx5 Core/Config >= 5.1.22 and relevant core module interface headers,
LibIME Pinyin >= 1.1.14, Boost headers, POSIX threads, libcurl, nlohmann-json >=
3.2 and gettext tools (`msgcat`/`msgfmt`). Tests additionally require Fcitx
TestFrontend development files. The Chinese translation test needs an available
UTF-8 locale such as `zh_CN.UTF-8` or `en_US.UTF-8`. Qt, ECM, LibIME Table and
OpenCC are not required by the LLM-only build.

```sh
cmake -S . -B build-llm -DENABLE_CHINESE_LLM_ONLY=ON -DENABLE_TEST=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
  -DFCITX_INSTALL_USE_FCITX_SYS_PATHS=OFF
cmake --build build-llm -j2
# Stage only our component; never install the full upstream addon set:
DESTDIR="$PWD/package-stage" cmake --install build-llm --component chinese-llm
```

The build generates audited private variants of the upstream sources in its build
tree. Explicit transformations rename paths, symbols, properties/actions and
loaders. Unexpected source changes fail configuration rather than silently
sharing official state. Upstream `im/pinyin` and helper module sources are not
edited. Dictionary resources are bundled or generated from local sources; the
LLM-only configure/build path does not download data or a model.

The `chinese-llm` component installs only its library, four private helper
libraries, their uniquely named metadata, the `llm-pinyin` entry and private
read-only resources, plus the private `chinese-llm` Simplified Chinese translation
catalog. It includes native pinyin labels as well as all LLM settings/tooltips,
and never installs or binds the official `fcitx5-chinese-addons` catalog. English
remains the fallback for other languages. The package checks an exact 21-file
allowlist and refuses unexpected upstream addon files, symlinks or auto-enabling
the main LLM addon.

### Arch Linux

The included `PKGBUILD` packages the **current local source**, including new and
uncommitted files. It does not fetch an old remote commit. From this checkout:

```sh
makepkg -s
```

This builds/tests/packages, but does not install the result. `-s` may install
missing dependencies via pacman; source/model downloads are not part of the
recipe. To keep output outside the checkout:

```sh
mkdir -p /absolute/path/to/package-work
cp PKGBUILD /absolute/path/to/package-work/
cd /absolute/path/to/package-work
CHINESE_LLM_SOURCE_DIR=/absolute/path/to/chinese-llm-addons makepkg -s
```

`prepare()` snapshots allowlisted source/resources under makepkg's `srcdir` and
excludes `.git`, existing builds, `pkg`, `src` and caches. Do not edit source while
it snapshots. The version is `5.1.15.local.h<source-content-hash>`; makepkg updates
`pkgver` and resets `pkgrel` to 1 on version changes. Normal preparation creates a
fresh snapshot; `--noextract` deliberately reuses an existing one.

There are no `provides`, `conflicts` or `replaces` for official Chinese addons.
Keep your existing package/entries if you use them. Inspect the generated package
with `pacman -Qlp` before installing via `pacman -U`; never use `--overwrite` to
force replacement of original addons.

The full upstream CMake mode remains available by omitting
`ENABLE_CHINESE_LLM_ONLY`; it requires the upstream optional dependencies. Do not
use unrestricted full `cmake --install` when you only want our addon. Use
`--component chinese-llm` even in a full build.

## Enable and configure in the GUI

1. Install the new package/component, then restart Fcitx5.
2. In **Fcitx Configuration → Addons**, enable **LLM拼音**, apply, and restart
   Fcitx5 again when prompted. The main addon remains disabled by default.
3. In **Input Method**, add **LLM拼音** without removing original 拼音/双拼.
4. Select LLM拼音 and open its settings gear.
   - The main page configures **LLM** settings: endpoint, model, token, prompts,
     debounce, candidate position, context/cache and cooldown.
   - **Independent Pinyin Settings** opens its **own native** configuration,
     not the original pinyin's. Private punctuation/conversion links also target
     only private addons.

Adding an entry alone does **not** enable its addon. Some configtool versions
show a blank dialog if the disabled addon cannot answer the configuration request.
Close it, enable the addon in **Addons**, apply/restart, then reopen the gear.

Installing/enabling the addon does not start a model server. Requests require
LLM拼音 to be active, its own LLM `Enabled=True` and a valid eligible composition.
A disabled LLM switch or failed model request leaves its private native dictionary
input fully usable. Switching to original pinyin is always model-free.

### Candidate cloud marker and visible LLM switch

With `ShowBadge=True`, model-generated candidates show a small monochrome cloud
comment, for example `她们正在开会 ☁︎`. Fcitx Kimpanel sends the candidate's
`textWithComment()` as plain display text; GNOME Kimpanel displays that string
without needing an extension patch. The cloud is not committed, cached as model
text or learned. Its exact appearance depends on the panel font. Turning the
marker off removes it without disabling LLM requests.

`CandidateIndex=1` inserts model suggestions in the first slot **after a result
arrives**. A first dictionary candidate is not evidence of model output. Results
identical to existing dictionary candidates are deduplicated; the dictionary
entry remains unmarked and retains its native selection/learning behavior.

The LLM input method's status menu now contains **LLM candidates enabled** /
**LLM candidates disabled** (translated into Chinese). Click it or use the
configured toggle shortcut to change the same persisted `Enabled` setting.
Kimpanel does not transmit action checkbox state, so the visible menu text spells
out the state rather than relying on a checkmark. The GNOME top bar uses `AI`
when enabled and `PY` when disabled: its label renderer truncates ASCII labels to
two characters. Config changes, reloads and toggles refresh this status immediately.
The action appears only in LLM拼音, not original 拼音/双拼.

This is the **configured switch state**, not a server-health or request-progress
indicator. Short compositions, sensitive fields, failed requests, cooldown and
candidate deduplication may leave only native candidates even with `AI` enabled.

## OpenAI-compatible endpoint

`Endpoint` is the **complete Chat Completions URL**, not just a base URL or `/v1`.
Use the service's actual model id and API key if required:

```ini
# Ollama
Endpoint=http://127.0.0.1:11434/v1/chat/completions
Model=qwen3:0.6b

# llama.cpp / llama-server example instead:
# Endpoint=http://127.0.0.1:8080/v1/chat/completions
# Model=beam-0.6b
```

```ini
# Hosted compatible service example
Endpoint=https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions
Model=qwen-plus
ApiKey=your-token
```

Manual configuration is also supported in the private LLM config path. It is a
**flat INI** with no `[ChineseLlm]` section. API keys are stored in plain text;
leave `ApiKey` empty and use `FCITX_CHINESE_LLM_API_KEY` if preferred. GUI configuration
changes invalidate pending model results and cached candidates. There is no
connection-test button, automatic model listing or model-service startup yet.

### LLM settings

- `Enabled=True`: model suggestions switch, separate from addon enablement.
- `Endpoint`, `Model`, `ApiKey`: compatible API settings.
- `MinPinyinLength=4`, `DebounceMs=250`, `TimeoutSeconds=5`.
- `MaxCandidates=3`, `CandidateIndex=2` (1-based), `ShowBadge=True`.
- `ContextLength=64` (0 disables surrounding-text use), `CacheSize=256`.
- `MaxConsecutiveErrors=5`, `CooldownSeconds=120`.
- `ToggleKey=Control+Alt+Shift+L`: only intercepted in LLM拼音.
- `SystemPrompt`: request Chinese characters only, with no explanation.
- `PromptTemplate={context}\n{pinyin}`: placeholders for preceding text/raw keys.

Temperature 0 and `max_tokens=64` are currently fixed in code. The independent
entry currently uses full pinyin; obsolete `SkipShuangpin` keys are ignored.

## Candidate safety and timing

Requests are asynchronous and debounced. Reset, focus loss, switching input
methods, privacy changes, model/native configuration changes or toggling off
invalidate old results. Curl aborts stale transfers, and main-thread delivery and
selection recheck sequence, generation, composition, privacy and surrounding text.
Nothing is auto-committed: the user explicitly selects a candidate.

Output validation rejects invalid UTF-8, English/digits/punctuation-only text,
truncated completions and incompatible Han/syllable counts. It cannot guarantee
the model chose the correct pronunciation. Partial selections and immutable
stroke/action-filtered candidate lists suppress the model path. Raw server preedit
is preferred over Chinese client commit-preview text.

A timeout, dead endpoint, non-200 response or malformed JSON simply yields no
model suggestions; native candidates remain. Repeated errors enter a cooldown
rather than hammering the service.

## Tests

```sh
cmake -S . -B build-test -DENABLE_CHINESE_LLM_ONLY=ON -DENABLE_TEST=ON
cmake --build build-test -j2
ctest --test-dir build-test --output-on-failure
./scripts/smoke.sh build-test
```

Smoke additionally needs the Fcitx TestFrontend **runtime** module, Python3,
curl, Bash and GNU timeout. Original pinyin is needed only for coexistence
regressions, not for LLM拼音 runtime/standalone cases. The script does not rebuild;
finish building and freeze edits first. It uses isolated profile/config/data/cache
and its own local mock server, never the user's model endpoint or API token.

Regressions cover parsing, flat INI/schema export, worker cancellation/redirects,
LLM candidate commit/reset, privacy, preview, cache, cooldown, filtered stale
selection, virtual keyboard/click actions and modifier-only global-hotkey
precedence. Independence cases cover private native settings, custom phrases,
learning save/reload, feature actions and running with the original pinyin
engine disabled while original user-data sentinels remain unchanged.

CMake discovers testfrontend's runtime path; override
`FCITX5_TEST_ADDON_DIR`/`FCITX5_TEST_DATA_DIR` for unusual installations. Test
executables live under `build-test/bin`. Test metadata/resources live under the
build-tree test data directory. Smoke artifacts are retained under the printed
`build-test/smoke.*` directory. Timeouts/nonzero exits are failures, including
negative cases.

## Model notes

A small pinyin fine-tune can run locally, but prompt formats and candidate quality
vary. Adjust the prompts to match your model. Mock regressions prove transport,
lifecycle and isolation, not real-model quality or latency. Review model/dataset
licenses before redistribution; installing this addon does not download a model.
