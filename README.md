Chinese related addon for fcitx5
================================================
This provides pinyin and table input method support for fcitx5. Released
under LGPL-2.1+.

im/pinyin/emoji.txt is derived from Unicode CLDR with modification.

[![Jenkins Build](https://img.shields.io/jenkins/build?jobUrl=https%3A%2F%2Fjenkins.fcitx-im.org%2Fjob%2Ffcitx5-chinese-addons%2F)](https://jenkins.fcitx-im.org/job/fcitx5-chinese-addons/)

[![Coverity Scan Status](https://img.shields.io/coverity/scan/11995.svg)](https://scan.coverity.com/projects/fcitx-fcitx5-chinese-addons)

---

# LLM powered pinyin candidates (branch `feature/chinese-llm`)

This branch adds one optional addon, `chinese-llm`, on top of upstream. It
generates full sentence candidates for pinyin by calling a local or remote
OpenAI compatible chat completions endpoint, and inserts them into the candidate
list next to the usual dictionary candidates.

**Scope of the change.** The runtime addon is self-contained under
`modules/chinese-llm`; the upstream pinyin engine and cloudpinyin sources are
unchanged. Build integration, regression tests and scripts are also maintained
on this branch. The default CMake path still builds the upstream Chinese addons;
a separate LLM-only mode avoids rebuilding or installing their replacements.

## What it does

Type a pinyin string; after a short debounce the addon sends the keys plus the
text before the cursor to the endpoint, takes the reply, and inserts up to
`MaxCandidates` readings into the candidate list at `CandidateIndex`. Nothing is
ever committed automatically -- the user picks a candidate, same as any other.

That ordering matters. A 0.6B model reading initials like `nhsj` picks one
reading out of many ("你换手机" is just one), so the answer must be a suggestion,
never an auto-commit.

## Install

The LLM-only build needs a C++20-capable compiler, CMake (>= 3.20 for the
commands below), pkg-config,
Fcitx5 Core/Config development files >= 5.1.22 (including its CMake helpers),
POSIX threads, libcurl development files and nlohmann-json >= 3.2. It does **not**
need ECM, Qt, Boost, LibIME, OpenCC, gettext, dictionaries or the upstream addons'
module development headers. An installed Fcitx5 pinyin addon and dictionaries are
still required at **runtime**; keep your distribution's Chinese addons installed.

Package names vary by distribution. Typical LLM-only development dependencies:

```sh
# Arch Linux (install with your package manager as appropriate):
# base-devel cmake pkgconf fcitx5 curl nlohmann-json
# Runtime: fcitx5-chinese-addons
# Debian/Ubuntu:
# build-essential cmake pkg-config libfcitx5core-dev libfcitx5config-dev
# libfcitx5utils-dev libcurl4-openssl-dev nlohmann-json3-dev
# Runtime: fcitx5-pinyin (and its dictionary packages)

cmake -S . -B build-llm -DENABLE_CHINESE_LLM_ONLY=ON -DENABLE_TEST=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-llm -j2
# Run with the privileges needed for your prefix. Installs ONLY the LLM addon
# library and metadata, never replaces pinyin/table/cloudpinyin:
cmake --install build-llm --component chinese-llm
```

For an unprivileged staged install, configure with
`-DCMAKE_INSTALL_PREFIX="$PWD/stage" -DFCITX_INSTALL_USE_FCITX_SYS_PATHS=OFF`.
Fcitx must be configured to search that prefix's `lib/fcitx5` (or the configured
libdir) and `share/fcitx5`, for example via `FCITX_ADDON_DIRS` and
`FCITX_DATA_DIRS`; simply installing into an arbitrary prefix does not activate
an addon. `DESTDIR="$PWD/package" cmake --install build-llm --component chinese-llm`
can instead stage files for a package whose configured prefix is `/usr`.
Do not change an absolute Fcitx install path with `--prefix`; configure the prefix
and `FCITX_INSTALL_USE_FCITX_SYS_PATHS` before building.

If only a locally cached single header is available, add
`-DCHINESE_LLM_JSON_INCLUDE_DIR=/absolute/cache/include` where that directory
contains `nlohmann/json.hpp`. CMake verifies that it compiles and is >= 3.2.
The full upstream build also needs `nlohmann/json_fwd.hpp` and its included
headers; supply a complete nlohmann-json include tree there, not only one file.
Normally use your distribution's nlohmann-json CMake package; configuration never
downloads dependencies implicitly.

**Full upstream build:** omit `ENABLE_CHINESE_LLM_ONLY` (default `OFF`). In
addition to the above, this path needs ECM >= 1.0, gettext, Boost headers >= 1.61,
LibIME Pinyin/Table >= 1.1.14 and Fcitx5 Notifications/QuickPhrase/Spell/Clipboard
module development headers. Defaults also require OpenCC >= 1.0.1, Qt6 Concurrent,
Fcitx5 Qt6 Widgets Addons and Qt6 WebEngineWidgets; use `ENABLE_GUI=OFF` to skip Qt
or `ENABLE_BROWSER=OFF` to skip WebEngine. Other upstream optional dependencies
and data-generation tools remain governed by their existing CMake options.
`ENABLE_CHINESE_LLM=OFF` skips only the new addon. Do not use an unrestricted full
`cmake --install` if you only intend to install the LLM addon; use the component.

Then enable **Chinese LLM** in `fcitx5-configtool` -> Addons. It is disabled by
default; nothing is sent anywhere until you enable it.

## Configure

Edit `~/.config/fcitx5/conf/chinese-llm.conf` (or the corresponding
`FCITX_CONFIG_HOME/conf` path), then reload Fcitx configuration/restart it. The
addon configuration is a **flat INI file**: keys are at the root, with **no
`[ChineseLlm]` section**. Addon-manager enablement is separate from the
configuration's `Enabled=True` setting. The only mandatory setting changes are
`Endpoint` and, when needed, `Model`.

**ollama**

```ini
Endpoint=http://127.0.0.1:11434/v1/chat/completions
Model=qwen3:0.6b
```

**llama.cpp (`llama-server`)**

```sh
llama-server -m beam-0.6b-q8_0.gguf --port 8080
```

```ini
Endpoint=http://127.0.0.1:8080/v1/chat/completions
Model=beam-0.6b
```

**A hosted OpenAI compatible API**

```ini
Endpoint=https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions
Model=qwen-plus
ApiKey=sk-...
```

API keys are stored in plain text in that file. If you would rather not put one
there, leave `ApiKey` empty and export `FCITX_CHINESE_LLM_API_KEY` instead.

## Settings

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | `True` | Master switch inside the addon |
| `Endpoint` | ollama | OpenAI compatible chat completions URL |
| `Model` | `qwen3:0.6b` | Model name passed through to the server |
| `ApiKey` | empty | Bearer token; empty means no `Authorization` header |
| `MinPinyinLength` | 4 | Do not ask about shorter input |
| `DebounceMs` | 250 | Wait this long after the last keystroke |
| `TimeoutSeconds` | 5 | Give up on a slow reply |
| `MaxCandidates` | 3 | How many readings to offer |
| `CandidateIndex` | 2 | 1 based position in the candidate list |
| `ContextLength` | 64 | Characters of preceding text to send, 0 disables |
| `CacheSize` | 256 | LRU entries keyed by pinyin and captured context; 0 disables caching |
| `MaxConsecutiveErrors` | 5 | Failures before pausing |
| `CooldownSeconds` | 120 | How long to pause after that |
| `ShowBadge` | `True` | Comment "LLM" next to these candidates |
| `SystemPrompt` | Chinese conversion instruction | System message sent to the model |
| `PromptTemplate` | `{context}\n{pinyin}` | User message template; INI stores the newline as an escaped `\\n` |
| `SkipShuangpin` | `True` | Shuangpin keys are not full pinyin |
| `ToggleKey` | `Control+Alt+Shift+L` | Toggle without opening the config |

## Design notes

**Timing is asynchronous and debounced.** The addon listens for the input panel
update that the pinyin engine emits after rebuilding the candidate list, waits
`DebounceMs`, then asks the worker for one request at a time. A burst of
keystrokes collapses into a single request for the final pinyin, and a reply that
arrives after the user typed on is discarded. Reset, toggling off and changed
configuration invalidate pending work; curl's progress callback aborts obsolete
transfers so newer input need not wait for their full timeout. Cancellation is
not counted as a model failure. Nothing in this path blocks the keystroke.

**Model output is validated before it can be shown.** `sanitize()` strips
surrounding quotes and trailing punctuation, accepts Han text (including
supplementary-plane Han), and permits limited separators such as an interpunct
inside a Chinese name. English answers, digits, invalid UTF-8 and punctuation-only
answers are rejected. When pinyin contains explicit syllable separators, the
**Han character count**, not the total number of codepoints, must match the
syllable count. Truncated model output (`finish_reason=length`) is a failure even
if the partial text looks Chinese. These checks prevent malformed output from
being offered; they cannot prove a model chose the correct reading.

**It stays out of the way elsewhere.** No request is made for password fields,
for input methods other than `pinyin`, for shuangpin (unless you re-enable it), or
once the user has started selecting characters -- the preedit becomes a Han and
pinyin mix at that point and the public API cannot split it reliably. Immutable
stroke/action-filtered candidate lists are left untouched, and candidate selection
rechecks the composition, privacy flags and generation before committing.

**The preedit is read from the composition, not its Chinese preview.** The
server-side preedit is preferred when present, with `clientPreedit` as a fallback.
This supports both composing-pinyin mode and commit-preview mode, where the
client preedit may already show Chinese while the server preedit still contains
the raw composition. Starting a partial selection invalidates pending model
results rather than treating the Han/pinyin mixture as a new request.

**Failures are silent.** A dead endpoint, a timeout, a non-200, an oversized
reply, or unparsable JSON all just yield no candidates. After
`MaxConsecutiveErrors` in a row the addon pauses for `CooldownSeconds` so a broken
endpoint is not hammered. Typing is unaffected throughout.

## Testing

Test builds additionally need Fcitx5's TestFrontend development interface. Smoke
runs require its **runtime** `libtestfrontend.so`, the installed pinyin addon and
dictionaries, Python3, curl, Bash and GNU `timeout` (coreutils). Some distributions
ship testfrontend separately or disable it when building Fcitx; having the CMake
interface package alone does not guarantee that its runtime module exists. Build
Fcitx with its testing addons enabled if your installation lacks it. LLM-only
mode does not require TestIM; the full upstream test suite does.

```sh
cmake -S . -B build-test -DENABLE_CHINESE_LLM_ONLY=ON -DENABLE_TEST=ON
cmake --build build-test -j2
# Backend parsing/validation and flat INI configuration tests, no network:
ctest --test-dir build-test --output-on-failure
# Or run build-test/bin/testllmbackend directly.

# Worker and addon integration, using an isolated local Python mock server:
./scripts/smoke.sh build-test
```

All test executables are under the build tree's `bin/`, not `test/`. Building
`testllmaddon` also builds the addon and copies its generated metadata into
`test/addon/`. CMake also generates testfrontend metadata there, independent of
whether a distribution installs its testing metadata under `share/fcitx5/testing`.
Smoke uses both build-tree and installed addon/data directories;
CMake discovers testfrontend's runtime directory, or override it with
`-DFCITX5_TEST_ADDON_DIR=/path/to/fcitx5` at configure time or
`FCITX5_TEST_ADDON_DIR=/path/to/fcitx5` when invoking the script. Override a
nonstandard installed data prefix with `FCITX5_TEST_DATA_DIR=/path/to/share/fcitx5`.
Extra runtime module/data directories may be supplied via `FCITX_ADDON_DIRS` and
`FCITX_DATA_DIRS` (colon separated).

The script deliberately **does not rebuild**: configure/build beforehand, then
run it without another process modifying the same build tree. It runs the worker
network/cancellation/redirect/error regressions and 16 addon scenarios: live
candidate/commit, debounce, disabled, dead endpoint, preview, reset, partial
selection, toggling off before/after a request starts, sensitive input before/after
a request starts, context, cache, cooldown, configuration invalidation and stale
selection through an immutable filtered list. Each scenario has fresh
config/data/cache, a bounded timeout, an exit-zero assertion, a final success
marker and server request-count assertions.
A nonzero exit or timeout is a failure even in a negative scenario. The mock binds
an ephemeral port by default (`PORT` can override it), verifies its own readiness
identity and reserves a separate non-listening port for deterministic refusal.
Logs and request snapshots remain in the printed `build-test/smoke.*` directory.
`SMOKE_TIMEOUT=30` sets the per-test watchdog in seconds.

The end to end assertions use `她们正在开会` rather than the more obvious
`他们正在开会`: the pinyin engine assembles the latter on its own, so only the
former is exclusive evidence that the model path ran.

## Notes on the model

`beam-0.6b` (a Qwen3-0.6B fine-tune) is a reasonable starting point: it takes
pinyin keys directly and is small enough to run on CPU. Check the model's expected
prompt format and adapt `SystemPrompt`/`PromptTemplate`; the generic OpenAI-chat
default template is not a guarantee of good conversions with every fine-tune.
Two caveats from its own
model card: it was trained partly on GeneInput, which is published without a
license, so evaluate that before redistributing anything derived from it; and it
reads at most about 40 key letters.

On an i7-13700 expect a few hundred milliseconds per sentence on CPU; a GPU is
comfortably faster. Since this is not on the keystroke path, the delay shows up
only as candidates appearing a moment later.

