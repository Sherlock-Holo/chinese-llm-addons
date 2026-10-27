# Independent LLM拼音 package built from this checkout. It installs next to the
# official fcitx5-chinese-addons and never replaces its addons or input methods.
#
# Deliberate differences from Arch's fcitx5-chinese-addons recipe:
#   * source: this working tree, not a signed upstream tag; prepare() snapshots
#     it and pkgver() hashes the build inputs (see below).
#   * no py_stroke/py_table source tarballs and no noextract: both .mb files are
#     vendored in modules/chinese-llm/native/data and pinned by SHA256 at
#     configure time, so packaging needs no download at all.
#   * no extra-cmake-modules/fcitx5-lua and no fcitx5-qt/opencc/qt6-webengine:
#     ENABLE_CHINESE_LLM_ONLY=ON turns off GUI, OpenCC, cloudpinyin, data and
#     tools, so those dependencies would be genuinely unused.
#   * no provides/conflicts/replaces: coexisting with the official package is
#     the whole point of this addon.
#
# Run `makepkg` here, or copy this file elsewhere and set CHINESE_LLM_SOURCE_DIR
# to this checkout. Only prepare() reads the tree; `--noextract` reuses the
# prepared snapshot for an incremental rebuild.
pkgname=chinese-llm-addons
pkgver=5.1.15.local.h5c57f73655873eaa
pkgrel=1
pkgdesc='Independent LLM Pinyin input method for Fcitx5 (keeps official Chinese addons)'
arch=('x86_64')
url='https://github.com/Sherlock-Holo/chinese-llm-addons'
license=('LGPL-2.1-or-later')
# xdg-utils is a runtime requirement: the history viewer is opened with xdg-open.
depends=('fcitx5>=5.1.22' 'libime>=1.1.14' 'curl' 'gcc-libs' 'glibc' 'xdg-utils')
# git is needed because prepare() snapshots the working tree, not a tarball.
# cmake 3.20 is the floor for `ctest --test-dir`.
makedepends=('cmake>=3.20' 'ninja' 'git' 'nlohmann-json' 'boost' 'pkgconf'
             'gettext' 'python>=3.8')
# A local package does not need a separate -debug split; strip still applies.
options=('!debug')
source=()
sha256sums=()

# Build inputs only. A whole-repo copy would also pull in gui/, icon/, tools/
# and most translations, so a README or docs edit would needlessly change the
# content hash; these paths keep the version tied to what is actually compiled
# and installed, while still picking up new files without editing this recipe.
_local_paths=(
  'CMakeLists.txt'
  'config.h.in'
  'cmake'
  'modules/chinese-llm'
  'modules/punctuation'
  'modules/fullwidth'
  'modules/chttrans'
  'modules/pinyinhelper'
  'modules/cloudpinyin/cloudpinyin_public.h'
  'im/pinyin'
  'po/zh_CN.po'
  'test'
  'scripts'
)

# One entry per build area: a truncated checkout must fail here instead of half
# way through compiling. The build itself validates everything else.
_local_required=(
  'CMakeLists.txt'
  'config.h.in'
  'cmake/FindPthread.cmake'
  'modules/chinese-llm/CMakeLists.txt'
  'modules/chinese-llm/chinese-llm.cpp'
  'modules/chinese-llm/llmbackend.cpp'
  'modules/chinese-llm/llmhistory.cpp'
  'modules/chinese-llm/llmworker.cpp'
  'modules/chinese-llm/chinese-llm.conf.in.in'
  'modules/chinese-llm/llm-pinyin.conf'
  'modules/chinese-llm/native/CMakeLists.txt'
  'modules/chinese-llm/native/generate.py'
  'modules/chinese-llm/native/reviewed-sources.json'
  'modules/chinese-llm/native/data/py_stroke.mb'
  'modules/chinese-llm/native/data/py_table.mb'
  'modules/chinese-llm/po/CMakeLists.txt'
  'modules/chinese-llm/po/zh_CN.po'
  'im/pinyin/pinyin.cpp'
  'im/pinyin/chaizi.txt'
  'modules/punctuation/punctuation.cpp'
  'modules/chttrans/chttrans.cpp'
  'modules/pinyinhelper/pinyinhelper.cpp'
  'modules/cloudpinyin/cloudpinyin_public.h'
  'po/zh_CN.po'
  'test/CMakeLists.txt'
  'test/addon/CMakeLists.txt'
  'test/testllmaddon.cpp'
  'scripts/smoke.sh'
  'scripts/mock_server.py'
)

# Exactly the private component's install output: five libraries, their unique
# metadata, the private llm-pinyin entry, the read-only resources and the
# private translation catalog. Nothing from the official package may sneak in.
_package_files=(
  'usr/lib/fcitx5/libchinese-llm.so'
  'usr/lib/fcitx5/libchinese-llm-chttrans.so'
  'usr/lib/fcitx5/libchinese-llm-fullwidth.so'
  'usr/lib/fcitx5/libchinese-llm-pinyinhelper.so'
  'usr/lib/fcitx5/libchinese-llm-punctuation.so'
  'usr/share/fcitx5/addon/chinese-llm.conf'
  'usr/share/fcitx5/addon/chinese-llm-chttrans.conf'
  'usr/share/fcitx5/addon/chinese-llm-fullwidth.conf'
  'usr/share/fcitx5/addon/chinese-llm-pinyinhelper.conf'
  'usr/share/fcitx5/addon/chinese-llm-punctuation.conf'
  'usr/share/fcitx5/chinese-llm/chaizi.dict'
  'usr/share/fcitx5/chinese-llm/symbols'
  'usr/share/fcitx5/chinese-llm/sp.dat.example'
  'usr/share/fcitx5/chinese-llm/chttrans/gbks2t.tab'
  'usr/share/fcitx5/chinese-llm/pinyinhelper/py_stroke.mb'
  'usr/share/fcitx5/chinese-llm/pinyinhelper/py_table.mb'
  'usr/share/fcitx5/chinese-llm/punctuation/punc.mb.zh_CN'
  'usr/share/fcitx5/chinese-llm/punctuation/punc.mb.zh_HK'
  'usr/share/fcitx5/chinese-llm/punctuation/punc.mb.zh_TW'
  'usr/share/fcitx5/inputmethod/llm-pinyin.conf'
  'usr/share/locale/zh_CN/LC_MESSAGES/chinese-llm.mo'
)

# One fixed snapshot per srcdir. A random name per run used to leave a complete
# build tree (~30 MiB each) behind in $srcdir; this is recreated instead, and
# needs no side file to tell later phases where the tree went.
_snapshot_path() {
  _snapshot="${srcdir:?}/${pkgname}.source"
  case "$_snapshot" in
    "$srcdir/$pkgname.source") ;;
    *) printf 'error: refusing unexpected snapshot path: %s\n' "$_snapshot" >&2; return 1 ;;
  esac
  if [[ ! -d "$_snapshot" || -L "$_snapshot" ]]; then
    printf 'error: %s is missing; run makepkg without --noextract first\n' "$_snapshot" >&2
    return 1
  fi
}

prepare() {
  local checkout="${CHINESE_LLM_SOURCE_DIR:-${startdir:?}}"
  checkout="$(cd -- "$checkout" && pwd -P)" || return 1
  # Require the work-tree root, not any directory inside one: git reports paths
  # relative to the invocation directory, so a nested checkout would make every
  # path below ambiguous.
  local toplevel
  toplevel="$(git -C "$checkout" rev-parse --show-toplevel 2>/dev/null)" || toplevel=""
  if [[ -z "$toplevel" || "$(cd -- "$toplevel" && pwd -P)" != "$checkout" ]]; then
    printf 'error: %s is not the root of a git checkout, so the working tree\n' \
      "$checkout" >&2
    printf 'error: cannot be snapshotted reproducibly; point CHINESE_LLM_SOURCE_DIR\n' >&2
    printf 'error: at the chinese-llm-addons checkout\n' >&2
    return 1
  fi

  local required
  for required in "${_local_required[@]}"; do
    if [[ ! -f "$checkout/$required" || -L "$checkout/$required" ]]; then
      printf 'error: checkout lacks a regular source file: %s\n' "$required" >&2
      printf 'error: set CHINESE_LLM_SOURCE_DIR to the chinese-llm-addons checkout\n' >&2
      return 1
    fi
  done

  # The two vendored lookup tables stand in for upstream's py_stroke/py_table
  # tarballs. CMake pins their SHA256 (fail-closed), but check here too so a bad
  # placeholder file fails before a full build rather than at install time.
  local digest pair expected
  for pair in \
    'py_stroke.mb 9b526eab3ef2679d63155c8304fc4b11f701c2bf5db7946e7d3e940f54f3aaa9' \
    'py_table.mb 19b3e593e5a211fe548f39c894d9a2e2e07a08e3edee0083087b1471e5ba026b'; do
    expected="${pair##* }"
    digest="$(sha256sum -- "$checkout/modules/chinese-llm/native/data/${pair%% *}")" || return 1
    if [[ "${digest%% *}" != "$expected" ]]; then
      printf 'error: unreviewed private lookup data: %s\n' "${pair%% *}" >&2
      return 1
    fi
  done

  local snapshot="${srcdir:?}/${pkgname}.source"
  rm -rf -- "$snapshot" || return 1
  mkdir -p -- "$snapshot" || return 1

  # Tracked files plus untracked-but-not-ignored ones, so a new file is packaged
  # without touching this recipe or committing first. --null keeps odd names safe.
  local index="${srcdir:?}/${pkgname}.files"
  if ! git -C "$checkout" ls-files -z --cached --others --exclude-standard -- \
      "${_local_paths[@]}" >"$index"; then
    printf 'error: unable to enumerate the checkout contents\n' >&2
    return 1
  fi
  if [[ ! -s "$index" ]]; then
    printf 'error: the checkout exposed no build inputs\n' >&2
    return 1
  fi
  # Fail loudly, never silently drop: an entry under pkg/, src/ or a build tree
  # means _local_paths was widened, and packaging build output is always wrong.
  local entry
  while IFS= read -r -d '' entry; do
    case "$entry" in
      pkg/*|src/*|build*/|*/build/*|*.pkg.tar.*|PKGBUILD|.SRCINFO|*'~'|*.orig|*.rej)
        printf 'error: refusing to snapshot a build artifact: %s\n' "$entry" >&2
        return 1
        ;;
    esac
  done <"$index"
  # tar preserves modes and symlinks and detects a partial copy, unlike a loop of
  # individual cp calls.
  if ! ( set -o pipefail
         tar -C "$checkout" --null -T "$index" -cf - | tar -C "$snapshot" -xf -
       ); then
    printf 'error: unable to snapshot the checkout\n' >&2
    return 1
  fi
  rm -f -- "$index"

  if [[ ! -f "$snapshot/modules/chinese-llm/llm-pinyin.conf" &&
        ! -f "$snapshot/modules/chinese-llm/llm-pinyin.conf.in" ]]; then
    printf 'error: snapshot lacks the LLM拼音 input-method metadata\n' >&2
    return 1
  fi

  # Hash the copied files, including uncommitted work. Archive ordering,
  # timestamps and ownership are normalized so the version depends only on
  # content, never on mtimes or the absolute checkout path.
  local digest
  if ! digest="$(set -o pipefail
      tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
        -cf - -C "$snapshot" . | sha256sum
    )"; then
    printf 'error: unable to hash the snapshot\n' >&2
    return 1
  fi
  printf '5.1.15.local.h%s\n' "${digest:0:16}" >"$snapshot/.local-version" || return 1
  printf 'snapshot: %s files under %s\n' \
    "$(find "$snapshot" -type f | wc -l)" "$snapshot"
}

pkgver() {
  _snapshot_path || return 1
  local version
  IFS= read -r version <"$_snapshot/.local-version" || return 1
  printf '%s\n' "$version"
}

build() {
  _snapshot_path || return 1
  # ENABLE_CHINESE_LLM_ONLY=ON already forces OpenCC, GUI, browser, cloudpinyin,
  # data and tools off, so only the switches that matter are passed explicitly.
  local -a args=(
    -S "$_snapshot"
    -B "$_snapshot/build"
    -G Ninja
    -DCMAKE_BUILD_TYPE=None
    -DCMAKE_INSTALL_PREFIX=/usr
    -DCMAKE_INSTALL_LIBDIR=lib
    -DFCITX_INSTALL_USE_FCITX_SYS_PATHS=OFF
    -DENABLE_CHINESE_LLM=ON
    -DENABLE_CHINESE_LLM_ONLY=ON
    -DENABLE_TEST=ON
  )
  # Offline escape hatch for a header-only nlohmann/json that has no CMake
  # package. The option is validated at configure time; no path is baked in here.
  if [[ -n "${CHINESE_LLM_JSON_INCLUDE_DIR:-}" ]]; then
    args+=("-DCHINESE_LLM_JSON_INCLUDE_DIR=$CHINESE_LLM_JSON_INCLUDE_DIR")
  fi
  cmake "${args[@]}" || return 1
  cmake --build "$_snapshot/build"
}

check() {
  _snapshot_path || return 1
  # Every ctest-registered test: deterministic and network-free. testllmworker
  # and testllmaddon are intentionally not registered there because they need a
  # mock HTTP server and a live Fcitx instance; run scripts/smoke.sh against a
  # configured build tree for those.
  ctest --test-dir "$_snapshot/build" --output-on-failure --no-tests=error
}

package() {
  _snapshot_path || return 1
  if [[ ! -d "${pkgdir:?}" ]]; then
    printf 'error: package directory is missing: %s\n' "$pkgdir" >&2
    return 1
  fi
  # Stage into the package, never into the running system. The component is an
  # additional safeguard even in LLM-only mode.
  DESTDIR="$pkgdir" cmake --install "$_snapshot/build" --component chinese-llm || return 1

  # Inspect the staged tree instead of trusting cmake: every file must be one of
  # the expected private files, and symlinks/devices are refused outright rather
  # than silently dereferenced. Directories are allowed; only files are counted.
  local -a staged=()
  mapfile -d '' -t staged < <(
    cd "${pkgdir:?}" || exit 1
    find . -mindepth 1 -printf '%y\t%P\0'
  ) || return 1
  local -A wanted=()
  local path
  for path in "${_package_files[@]}"; do
    wanted["$path"]=1
    if [[ ! -f "$pkgdir/$path" || -L "$pkgdir/$path" ]]; then
      printf 'error: expected a regular package file: %s\n' "$path" >&2
      return 1
    fi
  done
  local entry kind count=0
  for entry in "${staged[@]}"; do
    kind="${entry%%$'\t'*}"
    path="${entry#*$'\t'}"
    case "$kind" in
      d) continue ;;
      f) ;;
      *)
        printf 'error: refusing %s entry: %s\n' \
          "$([[ "$kind" == l ]] && echo symlink || echo non-regular)" "$path" >&2
        return 1
        ;;
    esac
    if [[ -z "${wanted[$path]:-}" ]]; then
      printf 'error: refusing unexpected package file: %s\n' "$path" >&2
      return 1
    fi
    count=$((count + 1))
  done
  if (( count != ${#_package_files[@]} )); then
    printf 'error: expected exactly %d private LLM files, found %d\n' \
      "${#_package_files[@]}" "$count" >&2
    return 1
  fi

  # Installing the package must not automatically enable model requests: require
  # one disabled setting in [Addon], not an unrelated section or a duplicate key.
  local enabled
  enabled="$(awk '/^\[/ { in_addon = ($0 == "[Addon]"); next }
                  in_addon && /^Enabled=/ { print }' \
    "$pkgdir/usr/share/fcitx5/addon/chinese-llm.conf")" || return 1
  if [[ "$enabled" != 'Enabled=False' ]]; then
    printf 'error: refusing an addon not unambiguously disabled by default\n' >&2
    return 1
  fi
  # The new basename must bind to the private addon, never to the official one.
  if ! grep -qx 'Addon=chinese-llm' \
      "$pkgdir/usr/share/fcitx5/inputmethod/llm-pinyin.conf"; then
    printf 'error: llm-pinyin is not bound to the private addon\n' >&2
    return 1
  fi
}
