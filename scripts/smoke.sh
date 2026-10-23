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
for name in testllmbackend testllmworker testllmaddon; do
    [[ -x "$BUILD_DIR/bin/$name" ]] || {
        echo "error: missing $BUILD_DIR/bin/$name; build with ENABLE_TEST=ON" >&2; exit 1;
    }
done
[[ -f "$BUILD_DIR/test/addon/chinese-llm.conf" ]] || {
    echo "error: missing build-tree addon metadata; build the testllmaddon target" >&2; exit 1;
}

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
run_checked worker 'All worker tests passed' "$RUN_DIR/worker.log" \
    "$BUILD_DIR/bin/testllmworker" "$BASE/v1/chat/completions"

for scenario in live debounce disabled dead preview reset partial toggle toggle-inflight sensitive sensitive-inflight context cache cooldown config filtered; do
    scenario_dir="$RUN_DIR/$scenario"
    mkdir -p "$scenario_dir"/{config/conf,data,cache,runtime}
    chmod 700 "$scenario_dir/runtime"
    # An explicit profile avoids host preferences and first-run IM selection.
    printf '%s\n' '[Groups/0]' 'Name=Default' 'Default Layout=us' 'DefaultIM=pinyin' \
        '' '[Groups/0/Items/0]' 'Name=pinyin' 'Layout=' \
        '' '[GroupOrder]' '0=Default' >"$scenario_dir/config/profile"
    curl --noproxy '*' --fail --silent --max-time 2 -X POST "$BASE/reset" > /dev/null
    endpoint="$BASE/v1/chat/completions"
    case "$scenario" in
        dead) endpoint="http://127.0.0.1:$DEAD_PORT/v1/chat/completions" ;;
        cooldown) endpoint="$BASE/error/v1/chat/completions" ;;
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
            "reset": 2, "context": 2, "cache": 2, "cooldown": 2, "config": 2, "filtered": 1}
if scenario in expected:
    assert count == expected[scenario], f"{scenario}: expected {expected[scenario]} POSTs, got {count}"
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
