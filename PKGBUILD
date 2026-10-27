# Local-checkout LLM拼音 package; coexists with fcitx5-chinese-addons.
# Run makepkg from this checkout, or set CHINESE_LLM_SOURCE_DIR to it.
# prepare() includes uncommitted source files in a fresh, isolated snapshot.
pkgname=chinese-llm-addons
pkgver=5.1.15.local.h37bc548cce9c5355
pkgrel=1
pkgdesc='Independent LLM Pinyin input method for Fcitx5 (keeps official Chinese addons)'
arch=('x86_64')
url='https://github.com/Sherlock-Holo/chinese-llm-addons'
license=('LGPL-2.1-or-later')
depends=('fcitx5>=5.1.22' 'libime>=1.1.14' 'curl' 'gcc-libs' 'glibc' 'xdg-utils')
makedepends=('cmake>=3.12' 'ninja' 'nlohmann-json' 'pkgconf' 'boost' 'python>=3.8' 'gettext')
options=('!debug')
# No provides/conflicts/replaces for fcitx5-chinese-addons or fcitx.
# This recipe intentionally packages local sources, not an obsolete remote pin.
# Dependency installation via makepkg -s may still use the package manager/network.
source=()
sha256sums=()

# makepkg runs phase functions separately; persist the chosen fresh source path
# rather than relying on a variable set by prepare() surviving into build().
_snapshot_root() {
  IFS= read -r _snapshot < "${srcdir:?}/${pkgname}.source-path" || return 1
  case "$_snapshot" in
    "$srcdir/$pkgname.snapshot."*) ;;
    *) printf 'error: invalid local snapshot path\n' >&2; return 1 ;;
  esac
  [[ -d "$_snapshot" && ! -L "$_snapshot" ]] || return 1
}

prepare() {
  local checkout="${CHINESE_LLM_SOURCE_DIR:-${startdir:?}}"
  checkout="$(cd -- "$checkout" && pwd -P)" || return 1
  local required
  for required in CMakeLists.txt config.h.in cmake/FindPthread.cmake \
    modules/chinese-llm/CMakeLists.txt modules/chinese-llm/chinese-llm.cpp \
    modules/chinese-llm/chinese-llm.h modules/chinese-llm/chinese-llm.conf.in.in \
    modules/chinese-llm/llmbackend.cpp modules/chinese-llm/llmbackend.h \
    modules/chinese-llm/llmworker.cpp modules/chinese-llm/llmworker.h \
    modules/chinese-llm/llmhistory.cpp modules/chinese-llm/llmhistory.h \
    test/testllmhistory.cpp test/CMakeLists.txt test/testdir.h.in test/addon/CMakeLists.txt \
    test/testllmbackend.cpp test/testllmconfig.cpp test/testllmworker.cpp \
    test/testllmaddon.cpp scripts/smoke.sh scripts/mock_server.py \
    modules/chinese-llm/native/CMakeLists.txt modules/chinese-llm/native/generate.py \
    modules/chinese-llm/native/reviewed-sources.json \
    modules/chinese-llm/po/CMakeLists.txt modules/chinese-llm/po/zh_CN.po po/zh_CN.po \
    modules/chinese-llm/native/data/py_stroke.mb modules/chinese-llm/native/data/py_table.mb \
    im/pinyin/pinyin.cpp im/pinyin/pinyin.h im/pinyin/chaizi.txt im/pinyin/symbols \
    im/pinyin/sp.dat.example modules/cloudpinyin/cloudpinyin_public.h \
    modules/punctuation/punctuation.cpp modules/punctuation/punctuation.h \
    modules/punctuation/punctuation_public.h modules/punctuation/punc.mb.zh_CN \
    modules/punctuation/punc.mb.zh_HK modules/punctuation/punc.mb.zh_TW \
    modules/fullwidth/fullwidth.cpp modules/fullwidth/fullwidth.h \
    modules/chttrans/chttrans.cpp modules/chttrans/chttrans.h \
    modules/chttrans/chttrans-native.cpp modules/chttrans/chttrans-native.h \
    modules/chttrans/gbks2t.tab modules/pinyinhelper/pinyinhelper.cpp \
    modules/pinyinhelper/pinyinhelper.h modules/pinyinhelper/pinyinhelper_public.h \
    modules/pinyinhelper/pinyinlookup.cpp modules/pinyinhelper/pinyinlookup.h \
    modules/pinyinhelper/stroke.cpp modules/pinyinhelper/stroke.h; do
    if [[ ! -f "$checkout/$required" || -L "$checkout/$required" ]]; then
      printf 'error: local checkout lacks a regular source file: %s\n' "$required" >&2
      return 1
    fi
  done
  if [[ ! -f "$checkout/modules/chinese-llm/llm-pinyin.conf.in" &&
        ! -f "$checkout/modules/chinese-llm/llm-pinyin.conf" ]]; then
    printf 'error: checkout lacks the new LLM拼音 input-method metadata\n' >&2
    return 1
  fi

  _snapshot="$(mktemp -d "${srcdir:?}/${pkgname}.snapshot.XXXXXX")" || return 1
  (
    cd -- "$checkout" || exit 1
    shopt -s nullglob
    local file
    # Flat source patterns exclude .git, build, pkg, src and Python caches even
    # when makepkg itself is running inside the checkout. Do not use git archive:
    # the new input-method metadata and other local edits need not be committed.
    local files=(CMakeLists.txt config.h.in cmake/*.cmake
      modules/chinese-llm/CMakeLists.txt modules/chinese-llm/*.cpp
      modules/chinese-llm/*.h modules/chinese-llm/*.in
      modules/chinese-llm/*.in.in modules/chinese-llm/*.conf
      modules/chinese-llm/po/CMakeLists.txt modules/chinese-llm/po/*.po po/zh_CN.po
      test/CMakeLists.txt test/testdir.h.in test/testllm*.cpp
      test/addon/CMakeLists.txt scripts/*.sh scripts/*.py
      modules/chinese-llm/native/CMakeLists.txt modules/chinese-llm/native/*.py
      modules/chinese-llm/native/*.json modules/chinese-llm/native/*.md
      modules/chinese-llm/native/data/py_stroke.mb modules/chinese-llm/native/data/py_table.mb
      im/pinyin/pinyin.cpp im/pinyin/pinyin.h im/pinyin/pinyincandidate.cpp
      im/pinyin/pinyincandidate.h im/pinyin/customphrase.cpp im/pinyin/customphrase.h
      im/pinyin/symboldictionary.cpp im/pinyin/symboldictionary.h
      im/pinyin/workerthread.cpp im/pinyin/workerthread.h
      im/pinyin/chaizi.txt im/pinyin/symbols im/pinyin/sp.dat.example
      modules/cloudpinyin/cloudpinyin_public.h
      modules/punctuation/punctuation.cpp modules/punctuation/punctuation.h
      modules/punctuation/punctuation_public.h modules/punctuation/punc.mb.zh_CN
      modules/punctuation/punc.mb.zh_HK modules/punctuation/punc.mb.zh_TW
      modules/fullwidth/fullwidth.cpp modules/fullwidth/fullwidth.h
      modules/chttrans/chttrans.cpp modules/chttrans/chttrans.h
      modules/chttrans/chttrans-native.cpp modules/chttrans/chttrans-native.h
      modules/chttrans/gbks2t.tab modules/pinyinhelper/pinyinhelper.cpp
      modules/pinyinhelper/pinyinhelper.h modules/pinyinhelper/pinyinhelper_public.h
      modules/pinyinhelper/pinyinlookup.cpp modules/pinyinhelper/pinyinlookup.h
      modules/pinyinhelper/stroke.cpp modules/pinyinhelper/stroke.h)
    for file in "${files[@]}"; do
      [[ -f "$file" && ! -L "$file" ]] || {
        printf 'error: refusing non-regular snapshot source: %s\n' "$file" >&2
        exit 1
      }
      cp --parents --preserve=mode,timestamps -- "$file" "$_snapshot" || exit 1
    done
  ) || return 1

  # Hash the actual copied files, including local/untracked changes. Normalize
  # archive ordering/timestamps/ownership so versioning does not depend on mtimes
  # or the absolute checkout path. Never claim this is the old remote snapshot.
  local digest
  digest="$(set -o pipefail
    tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
      -cf - -C "$_snapshot" . | sha256sum
  )" || return 1
  digest="${digest%% *}"
  printf '5.1.15.local.h%s\n' "${digest:0:16}" > "$_snapshot/.local-version" || return 1
  printf '%s\n' "$_snapshot" > "$srcdir/${pkgname}.source-path"
}

pkgver() {
  _snapshot_root || return 1
  local version
  IFS= read -r version < "$_snapshot/.local-version" || return 1
  printf '%s\n' "$version"
}

build() {
  _snapshot_root || return 1
  cmake -S "$_snapshot" -B "$_snapshot/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=None \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DFCITX_INSTALL_USE_FCITX_SYS_PATHS=OFF \
    -DENABLE_CHINESE_LLM=ON \
    -DENABLE_CHINESE_LLM_ONLY=ON \
    -DENABLE_TEST=ON \
    -DENABLE_GUI=OFF \
    -DENABLE_CLOUDPINYIN=OFF \
    -DENABLE_OPENCC=OFF \
    -DENABLE_DATA=OFF \
    -DENABLE_TOOLS=OFF || return 1
  cmake --build "$_snapshot/build"
}

check() {
  _snapshot_root || return 1
  # Deterministic, network-free tests; no real model or user's Fcitx session.
  ctest --test-dir "$_snapshot/build" --output-on-failure \
    -R '^testllm(backend|history|config|config-zh_CN|config-en)$'
}

package() {
  _snapshot_root || return 1
  # Stage into the package, never into the running system. The component is an
  # additional safeguard even in LLM-only mode; it must install exactly 21 files.
  DESTDIR="${pkgdir:?}" cmake --install "$_snapshot/build" --component chinese-llm || return 1
  (
    shopt -s globstar nullglob dotglob
    local path relative count=0
    for path in "$pkgdir"/**/*; do
      [[ -d "$path" && ! -L "$path" ]] && continue
      relative="${path#"$pkgdir"/}"
      case "$relative" in
        usr/lib/fcitx5/libchinese-llm.so|usr/share/fcitx5/addon/chinese-llm.conf|usr/share/fcitx5/inputmethod/llm-pinyin.conf|\
        usr/lib/fcitx5/libchinese-llm-punctuation.so|usr/share/fcitx5/addon/chinese-llm-punctuation.conf|\
        usr/lib/fcitx5/libchinese-llm-fullwidth.so|usr/share/fcitx5/addon/chinese-llm-fullwidth.conf|\
        usr/lib/fcitx5/libchinese-llm-chttrans.so|usr/share/fcitx5/addon/chinese-llm-chttrans.conf|\
        usr/lib/fcitx5/libchinese-llm-pinyinhelper.so|usr/share/fcitx5/addon/chinese-llm-pinyinhelper.conf|\
        usr/share/fcitx5/chinese-llm/symbols|usr/share/fcitx5/chinese-llm/sp.dat.example|usr/share/fcitx5/chinese-llm/chaizi.dict|\
        usr/share/fcitx5/chinese-llm/punctuation/punc.mb.zh_CN|usr/share/fcitx5/chinese-llm/punctuation/punc.mb.zh_HK|usr/share/fcitx5/chinese-llm/punctuation/punc.mb.zh_TW|\
        usr/share/fcitx5/chinese-llm/chttrans/gbks2t.tab|usr/share/fcitx5/chinese-llm/pinyinhelper/py_stroke.mb|usr/share/fcitx5/chinese-llm/pinyinhelper/py_table.mb|\
        usr/share/locale/zh_CN/LC_MESSAGES/chinese-llm.mo)
          if [[ ! -f "$path" || -L "$path" ]]; then
            printf 'error: expected a regular package file: %s\n' "$relative" >&2
            exit 1
          fi
          count=$((count + 1))
          ;;
        *)
          printf 'error: refusing unexpected package file: %s\n' "$relative" >&2
          exit 1
          ;;
      esac
    done
    if [[ "$count" -ne 21 ]]; then
      printf 'error: expected exactly twenty-one private LLM input-method files, found %s\n' "$count" >&2
      exit 1
    fi
  ) || return 1

  # Installing the package must not automatically enable model requests. Require
  # one disabled setting in [Addon], not an unrelated section or a duplicate key.
  local line in_addon=false enabled_count=0 disabled=false
  while IFS= read -r line; do
    line="${line%$'\r'}"
    if [[ "$line" == '[Addon]' ]]; then
      in_addon=true
    elif [[ "$line" == \[*\] ]]; then
      in_addon=false
    elif [[ "$in_addon" == true && "$line" == Enabled=* ]]; then
      enabled_count=$((enabled_count + 1))
      [[ "$line" == 'Enabled=False' ]] && disabled=true
    fi
  done < "$pkgdir/usr/share/fcitx5/addon/chinese-llm.conf"
  if [[ "$disabled" != true || "$enabled_count" -ne 1 ]]; then
    printf 'error: refusing an addon not unambiguously disabled by default\n' >&2
    return 1
  fi
}
