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

With `ShowOverlapBadge=True` (default), a **dictionary** candidate that the model
also produced gets a small monochrome gear (`你好 ⚙︎`). The two marks answer
different questions: the cloud says "this text exists only because of the model",
the gear says "the machine independently agrees with this dictionary candidate".
A check mark was tried first and dropped because people type it as real text, so
it read as a candidate; the intersection sign was dropped as too abstract. U+2699
has an emoji presentation, so the text variation selector (U+FE0E) requests the
monochrome glyph. Panels such as Kimpanel flatten a candidate's comment into plain
text, so the size cannot be changed; only the glyph and the font decide it.
When every model candidate duplicates a dictionary one, nothing is inserted, so
the mark is the only visible sign that the model answered. The mark is
display-only: it is not committed, not part of the text compared for statistics
and not learned. Turning the option off removes it on the next panel refresh and
does not reset your composition or affect statistics.

Both symbols are settings, so you can try others without rebuilding:
`BadgeSymbol` (default `☁︎`) for model candidates and `OverlapSymbol` (default `⚙︎`)
for agreeing dictionary candidates. **Clear the field and save to restore that
symbol's default**; the settings page then shows the default filled in again, and no
other setting is touched (unlike the page-wide "restore defaults"). Otherwise a value
must be valid UTF-8, at most four characters, with no control character, no `:` (the
Kimpanel property delimiter) and no blank at either end. An invalid value is
**rejected, not saved**: the previous symbol stays current, the configuration file
is not changed, and a desktop notification (when the notifications addon is
available) says which setting was refused and why. A value read from a hand-edited
file at startup that is invalid falls back to the default. The text is used exactly as
typed: nothing is appended, so a symbol with an emoji form is drawn in whichever
style the font picks unless you add U+FE0E (monochrome) or U+FE0F (colour) yourself.
Note that a symbol typed or pasted without U+FE0E can fall back to a font whose
line is much taller than Han text and stretch the candidate row (observed with a
bare U+2699 in Kimpanel: the default `⚙︎`, which carries U+FE0E, did not). If that
happens, clear the field to restore the default, add U+FE0E yourself, or pick
another symbol. A changed overlap symbol replaces the old one on the next panel refresh and
does not reset the composition. `BadgeSymbol` applies to candidates created
afterwards.

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

### Session request and overlap statistics

The LLM拼音 status menu also shows three read-only statistics, translated into
Chinese: **LLM requests N (valid V)**, **LLM overlapping requests O**, and
**LLM overlap rate R**. Numbers appear directly in each item's visible short
text because GNOME Kimpanel may not render tooltips. With no valid requests,
`R` is `—`, not `0%`; otherwise it is a percentage with one decimal place, such
as `33.3%`. The tooltips explain the counting rules below.

This is a **per-request any-match rate**, not a candidate-level percentage,
first-candidate agreement rate, model accuracy score or server-health indicator:

- **Requests (`sent`, N):** one logical transfer attempt is counted immediately
  before calling `curl_easy_perform`. HTTP redirects still count as just one
  attempt. A transfer that later fails or is cancelled remains counted; this is
  not proof that the server received a request. Cache hits and requests cancelled
  while still queued, before a transfer starts, do not increment the count.
- **Valid requests (`compared`, V):** a result is counted only when accepted on
  the main thread after the current sequence, generation, privacy, composition
  and surrounding-context guards pass, and when `ok` is true and its validated
  candidate list is nonempty. Errors, empty responses, results whose candidates
  are all filtered out, stale results and cancelled results do not enter this
  denominator. A successful HTTP response alone is not a valid request.
- **Overlapping requests (`overlapping`, O):** compare all validated model
  candidates with the **entire native modifiable candidate list**, using exact
  raw UTF-8 text equality. The comparison excludes additional LLM candidates;
  display comments, including the cloud marker, are not part of the text. Any
  model candidate matching any native candidate counts that valid request once,
  no matter how many candidates match. Mixed results with both native matches
  and novel candidates still count as overlapping, even when a cloud-marked
  novel candidate remains visible. Comparison happens before the model candidate
  display limit, so `MaxCandidates`, `ShowBadge` and `CandidateIndex` do not change
  the statistics.
- **Overlap rate (`R`):** `100 * overlapping / compared`. For example, one
  overlapping request among three valid requests gives `33.3%`, regardless of
  the total number of candidates those requests returned.

These counters belong to the current addon/Fcitx lifetime, not an individual
input field or model configuration. Configuration reloads, LLM toggles and input
method switches preserve them; restarting Fcitx resets them to zero. Statistics
persist neither input content nor counters to disk. This does not change the
separate native learning/history or in-memory LLM candidate-cache behavior.

### Clickable request history monitor

The LLM拼音 status menu has **LLM request history N** and **Clear LLM request
history**. Clicking the first opens a read-only local HTML snapshot in the default
browser (`xdg-open`), not a new HTTP service. Refresh the page or click the menu
again for updated records. Expand a row to inspect raw model text, accepted
candidates and rejected lines with their reason codes. Each row also shows input
keys, time/model, HTTP status, network-plus-parsing latency in milliseconds,
request outcome, exact native overlap and adoption. `llm` adoption means selecting
and committing a model candidate; `native` means committing an identical native
candidate. This tracks Fcitx commits, not acknowledgement by the destination app.

`RecordHistory=True` records the last 100 submitted requests in memory, plus cache
hits (clearly marked, with no network latency). Queued and cancelled attempts are
not successful model replies. `usable` means the current reply passed the main
thread's composition/privacy guards; `stale`, `cancelled`, `error`, `empty` and
`filtered` distinguish the reasons a result did not become a candidate. Filtering
reasons include invalid text, Han length mismatch, key echo, markup or list
lines, over-long replies and incomplete replies.
The aggregate **requests N / valid V** alone cannot distinguish these outcomes.
Cancellation retains its invalidating event (for example `focus_lost`,
`input_keys_or_context_changed`, `composition_reset` or
`surrounding_context_changed`) rather than only a generic `cancelled` message.
Repeated surrounding-text notifications with unchanged effective model context
preserve queued/in-flight requests and displayed candidates. A meaningful context
change cancels obsolete work and reschedules the still-current composition even
if the frontend emits no later input-panel update. Reset, focus loss, sensitive
fields and input-method changes remain hard invalidation boundaries.

No preceding context, API key, endpoint or request body is added to history. The
feature never sends history elsewhere and does not change native learning. Each
field is bounded: model text 8 KiB, displayed candidate/rejection/adoption text
1 KiB and at most 32 candidates/rejections per row; visible truncation markers
indicate omitted data. Original candidate identities for exact adoption matching
are separately bounded to 64 KiB per row. Old entries are evicted automatically;
cleared or evicted entries cannot reappear from late callbacks.

The history page is written in one language, never both. It is Chinese unless the
locale (`LANGUAGE`, `LC_ALL`, `LC_MESSAGES`, `LANG`, in that order) names a
non-Chinese language, in which case it is English. Machine-readable reasons such as
`input_keys_or_context_changed` are shown as a plain description with the original
code as a tooltip. Only a failed request is drawn in red; a cancellation or stale
reply is shown as a neutral reason.

By default no history file exists. Explicit viewing creates a snapshot in a private
0700 temporary directory with a 0600 file (XDG runtime directory, or a private
system-temp fallback). After opening, new records/commits update that snapshot;
rewrites are coalesced (about 300 ms) and not synced to disk, so typing never waits
for file I/O. Refresh the browser tab to see them.
Clearing history, disabling `RecordHistory`, entering a password/sensitive field
or unloading the addon clears history and removes the snapshot. An already-open
browser tab may retain its old view/cache; clear that separately if needed. A crash
can leave temporary files until runtime/temp cleanup. This is an opt-out,
normal-input in-memory monitor, not a permanent typing log. Toggle recording in
the addon settings; changing only that switch does not reset your composition.

If a model works via a manual request but not in the input method, inspect that
input's history row rather than assuming English was filtered. A manual request
with no context need not reproduce the actual Fcitx process's transport or
composition state. For a privacy-preserving comparison you can temporarily set
`ContextLength=0`; no private context needs to be shared for initial diagnosis.

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
- `MaxCandidates=3`, `CandidateIndex=2` (1-based), `ShowBadge=True`, `ShowOverlapBadge=True`, `BadgeSymbol=☁︎`, `OverlapSymbol=⚙︎`.
- `ContextLength=64` (0 disables surrounding-text use), `CacheSize=256`.
- `EnglishPolicy=Keep`: a drop-down for model results that contain Latin letters.
  `Keep` accepts them; `Discard` drops the whole result; `StripEdges` removes the
  English at the start and end of the result and keeps the rest (`Hello 你好吗,
  thanks!` becomes `你好吗`; English in the middle stays). A result with nothing
  left, and every dropped one, shows as `english_disabled` in the history page.
  A stripped result is no longer held to the one-Han-per-syllable rule, because
  the removed English took keys with it. Digits, punctuation and emoji are never
  English. Changing the setting clears cached results.
- `MaxConsecutiveErrors=5`, `CooldownSeconds=120`.
- `ToggleKey=Control+Alt+Shift+L`: only intercepted in LLM拼音.
- `SystemPrompt`: request candidate text only, preserving appropriate English,
  numbers and emoji, with no explanation or echoed input keys. Existing saved
  prompts are not overwritten; update a previously Chinese-only prompt in the
  GUI when using a general chat model. Beam's dedicated key-format template can
  ignore the system prompt.
- `PromptTemplate={context}\n{pinyin}`: placeholders for preceding text/raw keys.

Temperature 0 and `max_tokens=64` are currently fixed in code. The independent
entry currently uses full pinyin; obsolete `SkipShuangpin` keys are ignored.

## Candidate safety and timing

Requests are asynchronous and debounced. Reset, focus loss, switching input
methods, privacy changes, model/native configuration changes or toggling off
invalidate old results. A newer request aborts the transfer in flight immediately
(the worker is woken, not left to notice at curl's next progress tick, which on a
slow model is about a second later). Main-thread delivery and selection recheck
one input snapshot (generation, keys and context), privacy and eligibility.
Nothing is auto-committed: the user explicitly selects a candidate.

Output validation accepts Chinese/Latin text, numbers and visible emoji, including
mixed candidates such as `帮我push一下`, English such as `Hello world!`, and emoji
such as `👍🏽` or `👩‍💻`. Spaces and punctuation inside mixed/English results are
preserved, as are emoji presentation selectors, skin tones, ZWJ sequences, keycaps
and regional/subdivision flags. Pure Han results retain the legacy removal of
model-added edge quotes/punctuation and must still match a known syllable count.
That one-Han-per-syllable check does **not** apply to Latin/digit/emoji results;
instead, any result is bounded by the typed keys: it may not contain more Han
characters than typed letters, nor more Latin letters than typed letters plus four,
nor more than four times the typed letters plus sixteen characters in total.
Matching wrapper quotes or `**` emphasis around mixed text are removed.
Model candidates commit their text directly and do not enter native pinyin learning.

Validation still rejects invalid UTF-8/Unicode scalars, control characters (including
tabs, NUL and bare CR), bidi controls, unrelated invisible formatting, malformed or
standalone emoji joiners/selectors/tags, punctuation-only content, oversized replies
and explicitly incomplete completions. Chat-model narration is not a candidate:
`<think>…</think>` reasoning is removed from the reply before it is split, and lines
that contain markup tags, begin with a list marker such as `1.` or `- `, or end in
a colon (`Here is the conversion:`) are rejected. LF separates candidate lines; CRLF is also
accepted, but those separators are never committed. Exact input-key echoes are
rejected after ignoring ASCII case, spaces, apostrophes and model-added edge
wrappers/punctuation. A pure English output identical to its input keys is ambiguous
and is conservatively treated as an echo, not an exception to validation. The
validator cannot establish semantic correctness or guarantee that a nonidentical
English result is not a model explanation; keep prompts limited to candidate text.

Immutable stroke/action-filtered candidate lists suppress the model path.
Request eligibility still requires ASCII letter keys; accepting English, digits
and emoji **output** does not change the native engine's key handling.

Partial selections keep working. When you accept the first words of a long
sentence and keep typing the rest, the model is asked only about the pinyin that
is still unselected, and only up to the cursor, which is exactly what the
dictionary candidates on screen are for. That is what makes the agreement mark
meaningful there too. The words you already accepted are added to the end of the
context sent to the model (they are not in the application's text yet).
Choosing a model candidate commits the accepted words and the model's text
together, so the first half of the sentence is never lost. A remainder shorter
than `MinPinyinLength` sends nothing. The composition is read from the private
engine, not parsed from the preedit, which mixes hanzi and pinyin at that point.

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
regressions, not for the LLM-alone case. The script does not rebuild;
finish building and freeze edits first. It uses isolated profile/config/data/cache
and its own local mock server, never the user's model endpoint or API token.

Unit tests cover request parsing, flat INI/schema export, the sanitizer (Han, mixed,
narration and unsafe input), the worker (redirects, errors, immediate supersede,
reporting of dropped jobs) and the bounded, escaped history page. Ten real-addon
scenarios run a full Fcitx instance against the mock server: `live`, `mixed`,
`mixed-context-changed` (a changed surrounding context retries the final
composition), `cache`, `sensitive` (no request in password fields), `dead` (no
endpoint leaves native candidates intact), `toggle`, `history-menu` (private
snapshot, opener, adoption refresh, clear), `stats-overlap` (native adoption and
statistics) and `llm-alone` (runs with the original pinyin addon absent while its
user-data sentinels stay unchanged).

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
