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

**Scope of the change.** No existing file is modified beyond three `CMakeLists`
entries. `im/pinyin`, `modules/cloudpinyin` and every other addon are untouched,
so merging or rebasing onto upstream stays mechanical.

## What it does

Type a pinyin string; after a short debounce the addon sends the keys plus the
text before the cursor to the endpoint, takes the reply, and inserts up to
`MaxCandidates` readings into the candidate list at `CandidateIndex`. Nothing is
ever committed automatically -- the user picks a candidate, same as any other.

That ordering matters. A 0.6B model reading initials like `nhsj` picks one
reading out of many ("你换手机" is just one), so the answer must be a suggestion,
never an auto-commit.

## Install

```sh
sudo pacman -S --needed base-devel cmake pkgconf nlohmann-json   # Arch
# or: sudo apt install build-essential cmake pkg-config nlohmann-json3-dev

cmake -B build -DENABLE_GUI=Off -DENABLE_TEST=On
cmake --build build -j
sudo cmake --install build
```

Then enable **Chinese LLM** in `fcitx5-configtool` -> Addons. It is disabled by
default; nothing is sent anywhere until you enable it.

`ENABLE_GUI=Off` skips the Qt configuration GUI, which is unrelated to this
addon. `-DENABLE_CHINESE_LLM=Off` skips just this addon.

## Configure

`~/.config/fcitx5/conf/chinese-llm.conf`. The only mandatory change is
`Endpoint` (and `Model` if your server does not use the default).

**ollama**

```ini
[ChineseLlm]
Endpoint=http://127.0.0.1:11434/v1/chat/completions
Model=qwen3:0.6b
```

**llama.cpp (`llama-server`)**

```sh
llama-server -m beam-0.6b-q8_0.gguf --port 8080
```

```ini
[ChineseLlm]
Endpoint=http://127.0.0.1:8080/v1/chat/completions
Model=beam-0.6b
```

**A hosted OpenAI compatible API**

```ini
[ChineseLlm]
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
| `CacheSize` | 256 | LRU entries keyed by pinyin |
| `MaxConsecutiveErrors` | 5 | Failures before pausing |
| `CooldownSeconds` | 120 | How long to pause after that |
| `ShowBadge` | `True` | Comment "LLM" next to these candidates |
| `SkipShuangpin` | `True` | Shuangpin keys are not full pinyin |
| `ToggleKey` | `Control+Alt+Shift+L` | Toggle without opening the config |

## Design notes

**Timing is asynchronous and debounced.** The addon listens for the input panel
update that the pinyin engine emits after rebuilding the candidate list, waits
`DebounceMs`, then asks the worker for one request at a time. A burst of
keystrokes collapses into a single request for the final pinyin, and a reply that
arrives after the user typed on is discarded. Nothing in this path blocks the
keystroke.

**Model output is validated before it can be shown.** `sanitize()` in
`llmbackend.cpp` is the only gate. It takes each returned line, strips quotes and
trailing punctuation, rejects anything that is not Han text (so an English answer
or a digit cannot be committed), and -- when the pinyin has syllable separators --
requires the character count to match the syllable count exactly. The pinyin
engine checks that length itself only to decide whether to *learn* a word; the
commit path is unconditional, so a candidate one character too long would land in
the document with a wrong reading.

**It stays out of the way elsewhere.** No request is made for password fields,
for input methods other than `pinyin`, for shuangpin (unless you re-enable it), or
once the user has started selecting characters -- the preedit becomes a Han and
pinyin mix at that point and the public API cannot split it reliably.

**The preedit is read from whichever field the engine filled.** In
`ComposingPinyin` mode the engine writes `clientPreedit`; a client without the
`Preedit` capability gets `PreeditMode::No`, where the engine only fills the
server side preedit. Both are read, otherwise the addon would silently do nothing
in those clients.

**Failures are silent.** A dead endpoint, a timeout, a non-200, an oversized
reply, or unparsable JSON all just yield no candidates. After
`MaxConsecutiveErrors` in a row the addon pauses for `CooldownSeconds` so a broken
endpoint is not hammered. Typing is unaffected throughout.

## Testing

```sh
# unit tests only, no network
cmake --build build && ./build/test/testllmbackend

# full pipeline: builds a mock server, drives a real fcitx5 instance
./scripts/smoke.sh build
```

The smoke test covers a live endpoint, committing a candidate, and a dead
endpoint. It needs the `testfrontend` addon, which ships with fcitx5.

The end to end assertions use `她们正在开会` rather than the more obvious
`他们正在开会`: the pinyin engine assembles the latter on its own, so only the
former is exclusive evidence that the model path ran.

## Notes on the model

`beam-0.6b` (a Qwen3-0.6B fine-tune) is a reasonable starting point: it takes
pinyin keys directly and is small enough to run on CPU. Two caveats from its own
model card: it was trained partly on GeneInput, which is published without a
license, so evaluate that before redistributing anything derived from it; and it
reads at most about 40 key letters.

On an i7-13700 expect a few hundred milliseconds per sentence on CPU; a GPU is
comfortably faster. Since this is not on the keystroke path, the delay shows up
only as candidates appearing a moment later.

