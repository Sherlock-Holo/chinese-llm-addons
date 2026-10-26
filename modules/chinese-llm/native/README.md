# Private native-pinyin source reuse

This directory does not edit or load the official `pinyin` addon. At CMake
configuration time, `generate.py` reads the reviewed upstream files listed in
`reviewed-sources.json`, validates every SHA-256, and writes namespace/path-scoped
copies into the build tree. The pinyin factory is excluded. An object library is
linked into `libchinese-llm.so`; the wrapper owns that engine's lifetime. The
four helper copies are separate private addons, not shared official instances.

`python3 generate.py --source /path/to/checkout --check` performs all migrations
and assertions in memory without generating a build tree or compiling. A real
configuration also writes `generation-audit.json`, including source digests and
the cardinality of each replacement. Changed upstream inputs fail closed and
require review before updating the manifest. All reviewed source inputs and the
generator are CMake reconfigure dependencies.

- Native C++ namespace: `fcitx::llmnative`; public include:
  `native/pinyin/pinyin.h` relative to the LLM module's build directory.
- Private config: `conf/chinese-llm-pinyin.conf` and
  `conf/chinese-llm-{punctuation,fullwidth,chttrans}.conf`.
- Private user data: `chinese-llm/{user.dict,user.history,customphrase,dictionaries}`;
  the legacy shuangpin profile path, if consulted, is `chinese-llm/sp.dat` in
  package-config space. The wrapper exposes full pinyin only.
- Private property/action: `chinese-llm-pinyinState` and
  `chinese-llm-pinyin-prediction`; helper actions/properties have the same private
  prefix. All private helper observers/filters check the owning input-method
  identity; private punctuation/fullwidth/conversion do not intercept other IMs.
- Helper addons: `chinese-llm-{punctuation,fullwidth,chttrans,pinyinhelper}`. They
  are required by the main addon, are on-demand, and do not install original names.
  Runtime helper lookups use `load=false` because dependency loading precedes
  main-addon construction. PinyinHelper retains only readonly lookup/stroke
  functionality, with no QuickPhrase/clipboard provider registration.
- Shared mutable CloudPinyin, QuickPhrase, Spell and Lua IME API integrations are
  disabled at the dependency-loader boundary; their options are hidden/fixed off.
  Notifications may display messages through an already loaded core module,
  using private notification identifiers. This is not a separate Fcitx process:
  unrelated Fcitx/core/third-party global filters remain outside this ownership
  boundary.
- Chttrans uses only its private native conversion backend/table this iteration.
  OpenCC and its user profiles are not shared or required.
- Only installed readonly LibIME dictionaries/model may be reused. The language
  model is explicitly discovered from the installed LibIME layout through
  `CHINESE_LLM_SYSTEM_LANGUAGE_MODEL`, not the global user-search resolver.

## Data provenance

The reused source files retain their original copyright notices and
LGPL-2.1-or-later licensing. The repository's LGPL license and upstream resource
attribution continue to apply; generation is not a relicensing operation.
`symbols`, `sp.dat.example`, `chaizi.txt`, `punctuation/punc.mb.*` and
`chttrans/gbks2t.tab` come from this upstream Chinese-addons checkout. Chaizi is
built locally using LibIME's dictionary compiler, without a download. These
resources are installed under `share/fcitx5/chinese-llm`, never the original data
names. No original user's configuration or dictionaries are copied or migrated.

Two upstream PinyinHelper datasets are vendored in `data/` so the installed addon
has no dependency on `fcitx5-chinese-addons`. They were extracted as the only
regular member of the existing workspace cache after checking the archive SHA
against upstream `modules/pinyinhelper/CMakeLists.txt`:

| Vendored resource | Upstream archive URL | Archive SHA-256 | Extracted file SHA-256 |
|---|---|---|---|
| `py_stroke.mb` | `http://download.fcitx-im.org/data/py_stroke-20250329.tar.gz` | `c1a7ca7225d3614ab83353fc827503006a980447762018f53760425d7b5303a6` | `9b526eab3ef2679d63155c8304fc4b11f701c2bf5db7946e7d3e940f54f3aaa9` |
| `py_table.mb` | `http://download.fcitx-im.org/data/py_table-20121124.tar.gz` | `42146ac97de6c13d55f9e99ed873915f4c66739e9c11532a34556badf9792c04` | `19b3e593e5a211fe548f39c894d9a2e2e07a08e3edee0083087b1471e5ba026b` |

The reader/source attribution includes Yichao Yu and CSSlayer; original archives
and source notices are preserved in the upstream provenance above. CMake checks
the extracted-file digests before installing private copies. Neither configuration
nor the local PKGBUILD fetches those archives or depends on the original cache.
