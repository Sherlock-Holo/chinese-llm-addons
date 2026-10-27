#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Run a configured build, never build concurrently behind the caller's back.
# Usage: ./scripts/smoke.sh /absolute/or/relative/build-directory
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${1:-build}"
for tool in python3 curl timeout mktemp; do
    command -v "$tool" >/dev/null || { echo "error: missing $tool" >&2; exit 1; }
done
[[ -d "$BUILD_DIR" ]] || { echo "error: configure and build '$BUILD_DIR' first" >&2; exit 1; }
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
for name in testllmbackend testllmconfig testllmworker testllmaddon; do
    [[ -x "$BUILD_DIR/bin/$name" ]] || {
        echo "error: missing $BUILD_DIR/bin/$name; build with ENABLE_TEST=ON" >&2; exit 1;
    }
done
[[ -f "$BUILD_DIR/test/addon/chinese-llm.conf" &&
   -f "$BUILD_DIR/test/inputmethod/llm-pinyin.conf" ]] || {
    echo "error: missing build-tree addon/inputmethod metadata; build the testllmaddon target" >&2; exit 1;
}

for helper in punctuation fullwidth chttrans pinyinhelper; do
    [[ -f "$BUILD_DIR/test/addon/chinese-llm-$helper.conf" ]] || {
        echo "error: missing private helper metadata: chinese-llm-$helper" >&2; exit 1;
    }
done
for resource in symbols chaizi.dict punctuation/punc.mb.zh_CN chttrans/gbks2t.tab pinyinhelper/py_stroke.mb pinyinhelper/py_table.mb; do
    [[ -f "$BUILD_DIR/test/chinese-llm/$resource" ]] || {
        echo "error: missing private data: chinese-llm/$resource" >&2; exit 1;
    }
done

# Fcitx's module package exports interface headers, not its runtime .so path.
# CMake discovers the library directory; callers may override unusual layouts.
TEST_ADDON_DIR="${FCITX5_TEST_ADDON_DIR:-}"
if [[ -z "$TEST_ADDON_DIR" && -f "$BUILD_DIR/test/runtime-addon-dir.txt" ]]; then
    IFS= read -r TEST_ADDON_DIR < "$BUILD_DIR/test/runtime-addon-dir.txt" || true
fi
[[ -n "$TEST_ADDON_DIR" && -d "$TEST_ADDON_DIR" ]] || {
    echo "error: set FCITX5_TEST_ADDON_DIR to the directory containing libtestfrontend.so" >&2; exit 1;
}
if [[ ! -f "$TEST_ADDON_DIR/libtestfrontend.so" && ! -f "$TEST_ADDON_DIR/libtestfrontend.dylib" ]]; then
    echo "error: testfrontend runtime module not found in $TEST_ADDON_DIR" >&2; exit 1
fi
SYSTEM_DATA_DIR="${FCITX5_TEST_DATA_DIR:-}"
if [[ -z "$SYSTEM_DATA_DIR" && -f "$BUILD_DIR/test/runtime-data-dir.txt" ]]; then
    IFS= read -r SYSTEM_DATA_DIR < "$BUILD_DIR/test/runtime-data-dir.txt" || true
fi
[[ -d "$SYSTEM_DATA_DIR" ]] || {
    echo "error: set FCITX5_TEST_DATA_DIR to the installed share/fcitx5 directory" >&2; exit 1;
}
ADDON_DIRS="$BUILD_DIR/bin:$TEST_ADDON_DIR${FCITX_ADDON_DIRS:+:$FCITX_ADDON_DIRS}"
DATA_DIRS="$BUILD_DIR/test:$SYSTEM_DATA_DIR${FCITX_DATA_DIRS:+:$FCITX_DATA_DIRS}"

# Keep logs/config/data for diagnosis; each invocation and scenario is isolated.
RUN_DIR="$(mktemp -d "$BUILD_DIR/smoke.XXXXXX")"
echo "smoke artifacts: $RUN_DIR"
MOCK_PID=""
cleanup() {
    if [[ -n "$MOCK_PID" ]]; then
        kill "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
        MOCK_PID=""
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

TOKEN="${RUN_DIR##*/}-$RANDOM"
python3 "$HERE/mock_server.py" --port "${PORT:-0}" --delay 0.3 \
    --token "$TOKEN" --ready-file "$RUN_DIR/ready.json" >"$RUN_DIR/mock.log" 2>&1 &
MOCK_PID=$!
ready=false
for ((i=0; i<100; i++)); do
    kill -0 "$MOCK_PID" 2>/dev/null || break
    if [[ -s "$RUN_DIR/ready.json" ]]; then
        PORT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$RUN_DIR/ready.json")"
        if curl --noproxy '*' --fail --silent --max-time 1 "http://127.0.0.1:$PORT/healthz" \
            >"$RUN_DIR/health.json" && python3 -c \
            'import json,sys; assert json.load(open(sys.argv[1]))["token"] == sys.argv[2]' \
            "$RUN_DIR/health.json" "$TOKEN"; then
            ready=true
            break
        fi
    fi
    sleep 0.1
done
if [[ "$ready" != true ]]; then
    echo "error: our mock server did not start (port collision is not success)" >&2
    tail -n 50 "$RUN_DIR/mock.log" >&2
    exit 1
fi
BASE="http://127.0.0.1:$PORT"
DEAD_PORT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["dead_port"])' "$RUN_DIR/health.json")"
export NO_PROXY="127.0.0.1,localhost${NO_PROXY:+,$NO_PROXY}"
export no_proxy="$NO_PROXY"
# Never send a developer's real token to the mock or retain it in stats logs.
unset FCITX_CHINESE_LLM_API_KEY

failures=0
run_checked() {
    local label="$1" marker="$2" log="$3"
    shift 3
    local status=0
    timeout --kill-after=3s "${SMOKE_TIMEOUT:-30}s" "$@" >"$log" 2>&1 || status=$?
    if [[ "$status" -eq 0 ]] && python3 -c \
        'import sys; assert sys.argv[2] in open(sys.argv[1]).read().splitlines()' "$log" "$marker"; then
        echo "ok: $label"
    else
        echo "FAILED: $label (exit=$status; log=$log)" >&2
        tail -n 60 "$log" >&2
        failures=$((failures + 1))
    fi
}

# Backend has no network; worker covers network/cancel/redirect/error paths.
run_checked backend 'All tests passed' "$RUN_DIR/backend.log" "$BUILD_DIR/bin/testllmbackend"
run_checked config 'All config tests passed' "$RUN_DIR/config.log" "$BUILD_DIR/bin/testllmconfig"
run_checked worker 'All worker tests passed' "$RUN_DIR/worker.log" \
    "$BUILD_DIR/bin/testllmworker" "$BASE/v1/chat/completions"

for scenario in live debounce disabled dead preview reset partial toggle toggle-inflight sensitive sensitive-inflight context cache cooldown config filtered metadata original-pinyin original-shuangpin switch-inflight switch-results own-config virtual-keyboard preedit-click modifier-selection modifier-selection-global llm-alone private-settings customphrase-isolation learning-isolation learning-reload private-actions native-description cloud-badge badge-off llm-status stats-overlap stats-empty stats-error stats-invalid stats-all-filtered stats-limit stats-rate stats-focus; do
    scenario_dir="$RUN_DIR/$scenario"
    mkdir -p "$scenario_dir"/{config/conf,data,cache,runtime}
    chmod 700 "$scenario_dir/runtime"
    # An explicit profile avoids host preferences and first-run IM selection.
    default_im=llm-pinyin
    case "$scenario" in
        original-pinyin) default_im=pinyin ;;
        original-shuangpin) default_im=shuangpin ;;
    esac
    printf '%s\n' '[Groups/0]' 'Name=Default' 'Default Layout=us' "DefaultIM=$default_im" \
        '' '[Groups/0/Items/0]' 'Name=llm-pinyin' 'Layout=' \
        '' '[Groups/0/Items/1]' 'Name=pinyin' 'Layout=' \
        '' '[Groups/0/Items/2]' 'Name=shuangpin' 'Layout=' \
        '' '[GroupOrder]' '0=Default' >"$scenario_dir/config/profile"
    if [[ "$scenario" == llm-alone || "$scenario" == learning-isolation || "$scenario" == learning-reload ]]; then
        printf '%s\n' '[Groups/0]' 'Name=Default' 'Default Layout=us' 'DefaultIM=llm-pinyin' \
            '' '[Groups/0/Items/0]' 'Name=llm-pinyin' 'Layout=' \
            '' '[GroupOrder]' '0=Default' >"$scenario_dir/config/profile"
        python3 - "$scenario_dir" <<'PY'
import hashlib
import json
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
files = {
    "config/conf/pinyin.conf": "# official sentinel\nPageSize=9\nPrediction=False\n",
    "config/conf/punctuation.conf": "# official punctuation sentinel\n",
    "config/conf/fullwidth.conf": "# official fullwidth sentinel\n",
    "config/conf/chttrans.conf": "# official conversion sentinel\n",
    "config/conf/pinyinhelper.conf": "# official pinyinhelper sentinel\n",
    "config/pinyin/sp.dat": "official-shuangpin-sentinel\n",
    "data/pinyin/user.dict": "official-userdict-sentinel\n",
    "data/pinyin/user.history": "official-history-sentinel\n",
    "data/pinyin/customphrase": "zzzz,1=原生短语\n",
    "data/pinyin/dictionaries/sentinel.dict": "official-extradict-sentinel\n",
    "data/punctuation/punc.mb.zh_CN": "official-punctuation-data-sentinel\n",
    "data/chttrans/gbks2t.tab": "official-conversion-data-sentinel\n",
    "data/pinyinhelper/py_stroke.mb": "official-stroke-data-sentinel\n",
    "data/pinyinhelper/py_table.mb": "official-py-table-data-sentinel\n",
}
checksums = {}
for name, value in files.items():
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value)
    checksums[name] = hashlib.sha256(path.read_bytes()).hexdigest()
(root / "official-sentinels.json").write_text(json.dumps(checksums))
PY
        if [[ "$scenario" == learning-reload ]]; then
            mkdir -p "$scenario_dir/data/chinese-llm"
            cp "$RUN_DIR/learning-isolation/data/chinese-llm/user.dict" \
               "$RUN_DIR/learning-isolation/data/chinese-llm/user.history" "$scenario_dir/data/chinese-llm/"
        fi
    elif [[ "$scenario" == customphrase-isolation ]]; then
        mkdir -p "$scenario_dir/data/pinyin"
        printf '%s\n' 'zzzz,1=原生短语' >"$scenario_dir/data/pinyin/customphrase"
    fi
    curl --noproxy '*' --fail --silent --max-time 2 -X POST "$BASE/reset" > /dev/null
    endpoint="$BASE/v1/chat/completions"
    case "$scenario" in
        dead) endpoint="http://127.0.0.1:$DEAD_PORT/v1/chat/completions" ;;
        cooldown|stats-error) endpoint="$BASE/error/v1/chat/completions" ;;
        stats-empty) endpoint="$BASE/empty/v1/chat/completions" ;;
        stats-invalid) endpoint="$BASE/invalid/v1/chat/completions" ;;
        stats-all-filtered) endpoint="$BASE/all-filtered/v1/chat/completions" ;;
        stats-limit) endpoint="$BASE/late-overlap/v1/chat/completions" ;;
    esac
    # The mock reserves DEAD_PORT without listening, so connection refusal is
    # deterministic and never counted as test success solely due to a timeout.
    run_checked "addon/$scenario" "all addon tests passed: $scenario" "$scenario_dir/test.log" \
        env FCITX_ADDON_DIRS="$ADDON_DIRS" FCITX_DATA_DIRS="$DATA_DIRS" \
            FCITX_CONFIG_HOME="$scenario_dir/config" FCITX_DATA_HOME="$scenario_dir/data" \
            FCITX_CONFIG_DIRS="$scenario_dir/config" \
            XDG_CONFIG_HOME="$scenario_dir/config" XDG_DATA_HOME="$scenario_dir/data" \
            XDG_CACHE_HOME="$scenario_dir/cache" XDG_RUNTIME_DIR="$scenario_dir/runtime" \
            "$BUILD_DIR/bin/testllmaddon" "$endpoint" "--scenario=$scenario"
    curl --noproxy '*' --fail --silent --max-time 2 "$BASE/stats" >"$scenario_dir/stats.json"
    # Normal Fcitx shutdown saves the official pinyin addon too. Compare its
    # full settings against the pre-test baseline; do not flag file creation.
    # Every setup, including preview/click/modifiers, changes private config only.
    if ! python3 - "$scenario_dir" "$scenario" <<'PY'
import json
import pathlib
import sys
root = pathlib.Path(sys.argv[1])
scenario = sys.argv[2]
if scenario in {"llm-alone", "learning-isolation", "learning-reload"}:
    import hashlib
    checksums = json.loads((root / "official-sentinels.json").read_text())
    for name, expected_hash in checksums.items():
        assert hashlib.sha256((root / name).read_bytes()).hexdigest() == expected_hash, f"official sentinel changed: {name}"
    originals = set(checksums)
    for directory in ["data/pinyin", "config/pinyin", "data/punctuation", "data/chttrans", "data/pinyinhelper"]:
        assert all(str(path.relative_to(root)) in originals for path in (root / directory).rglob("*") if path.is_file()), f"new official data created: {directory}"
    assert (root / "config/conf/chinese-llm-pinyin.conf").is_file(), "private native configuration not saved"
    assert (root / "data/chinese-llm/user.dict").is_file() and (root / "data/chinese-llm/user.history").is_file(), "private learning files not saved"
    sys.exit(0)
prefix = "official-config-baseline: "
lines = (root / "test.log").read_text().splitlines()
baselines = [json.loads(line[len(prefix):]) for line in lines if line.startswith(prefix)]
assert len(baselines) == 1, "official configuration baseline missing or duplicated"
persisted = root / "config/conf/pinyin.conf"
assert persisted.is_file(), "official pinyin configuration was not saved on normal exit"
assert persisted.read_text() == baselines[0], "LLM entry overwrote official pinyin/shuangpin settings"
assert not (root / "config/conf/shuangpin.conf").exists(), "LLM unexpectedly wrote a separate shuangpin configuration"
PY
    then
        echo "FAILED: addon/$scenario changed official pinyin/shuangpin configuration" >&2
        failures=$((failures + 1))
    fi
    # Count assertions supplement in-process candidate/state assertions.
    if ! python3 - "$scenario" "$scenario_dir/stats.json" <<'PY'
import json
import sys
scenario = sys.argv[1]
stats = json.load(open(sys.argv[2]))
count = stats["count"]
expected = {"live": 1, "debounce": 1, "disabled": 0, "dead": 0,
            "preview": 1, "sensitive": 0, "toggle": 0, "partial": 1,
            "toggle-inflight": 1, "sensitive-inflight": 1,
            "reset": 2, "context": 2, "cache": 2, "cooldown": 2, "config": 2, "filtered": 1,
            "metadata": 0, "original-pinyin": 0, "original-shuangpin": 0,
            "switch-inflight": 2, "switch-results": 1, "own-config": 0,
            "virtual-keyboard": 1, "preedit-click": 1, "modifier-selection": 1,
            "modifier-selection-global": 1,
            "llm-alone": 1, "private-settings": 0, "customphrase-isolation": 0,
            "learning-isolation": 0, "learning-reload": 0, "private-actions": 0, "native-description": 0,
             "cloud-badge": 1, "badge-off": 1, "llm-status": 0,
             "stats-overlap": 1, "stats-empty": 1, "stats-error": 1,
             "stats-invalid": 1, "stats-all-filtered": 1, "stats-limit": 1,
             "stats-rate": 3, "stats-focus": 2}
assert scenario in expected, f"missing request-count assertion for {scenario}"
assert count == expected[scenario], f"{scenario}: expected {expected[scenario]} POSTs, got {count}"
if scenario in {"own-config", "metadata", "original-pinyin", "original-shuangpin"}:
    assert not stats["requests"], f"{scenario}: original/config-only path touched network"
print(f"  server requests: {count}")
PY
    then
        echo "FAILED: addon/$scenario server request assertions" >&2
        failures=$((failures + 1))
    fi
done
if [[ "$failures" -ne 0 ]]; then
    echo "$failures smoke check(s) failed; artifacts: $RUN_DIR" >&2
    exit 1
fi
echo "all smoke scenarios passed"
