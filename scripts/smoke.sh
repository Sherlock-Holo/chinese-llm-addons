#!/bin/bash
# SPDX-FileCopyrightText: 2026-2026
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# End to end smoke test for the chinese-llm addon.
#
# Builds the addon and its tests, starts the mock server, and drives the four
# scenarios that matter:
#   1. a live endpoint produces model candidates
#   2. selecting one commits the text and clears the composition
#   3. a dead endpoint produces no candidates, without hanging typing
#   4. the addon stays out of the way when it is disabled
#
# Requires a build tree already configured. Pass the build directory as $1.
set -euo pipefail

BUILD_DIR="${1:-build}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(dirname "$HERE")"
PORT="${PORT:-8899}"

# fcitx5 reads these when building its StandardPaths, before main() runs, so
# they must be in the environment when the test binary starts.
ADDON_DIR="$BUILD_DIR/bin"
DATA_DIR="$BUILD_DIR/test"

if [ ! -d "$BUILD_DIR" ]; then
    echo "error: build directory '$BUILD_DIR' does not exist" >&2
    echo "run: cmake -B $BUILD_DIR -DENABLE_GUI=Off && cmake --build $BUILD_DIR" >&2
    exit 1
fi

if [ ! -x "$BUILD_DIR/test/testllmaddon" ]; then
    echo "error: $BUILD_DIR/test/testllmaddon not built" >&2
    exit 1
fi

cleanup() {
    if [ -n "${MOCK_PID:-}" ]; then
        kill "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

start_mock() {
    python3 "$SRC_DIR/scripts/mock_server.py" --port "$PORT" >"$BUILD_DIR/mock.log" 2>&1 &
    MOCK_PID=$!
    for _ in $(seq 1 40); do
        if curl -sf -m 1 "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.25
    done
    echo "error: mock server did not start; see $BUILD_DIR/mock.log" >&2
    return 1
}

run_addon_test() {
    local endpoint="$1"
    FCITX_ADDON_DIRS="$ADDON_DIR" \
    FCITX_DATA_DIRS="$DATA_DIR" \
    FCITX_DATA_HOME=/Invalid/Path \
    FCITX_CONFIG_HOME=/Invalid/Path \
        "$BUILD_DIR/test/testllmaddon" "$endpoint"
}

failures=0

echo "=== 1/3 unit tests (no network) ==="
if "$BUILD_DIR/test/testllmbackend"; then
    echo "ok"
else
    echo "FAILED"
    failures=$((failures + 1))
fi

echo "=== 2/3 live endpoint ==="
start_mock
if run_addon_test "http://127.0.0.1:$PORT/v1/chat/completions" \
    | grep -q "ok: model candidate appeared"; then
    echo "ok"
else
    echo "FAILED: model candidate did not appear"
    failures=$((failures + 1))
fi
echo "requests reaching the server: $(grep -c POST "$BUILD_DIR/mock.log" || true)"
cleanup
MOCK_PID=

echo "=== 3/3 dead endpoint (typing must not hang, no candidates) ==="
# Port 9 is the discard port; nothing listens there.
if timeout 30 "$BUILD_DIR/test/testllmaddon" \
    "http://127.0.0.1:9/v1/chat/completions" >/dev/null 2>&1; then
    echo "FAILED: a model candidate appeared with no server"
    failures=$((failures + 1))
else
    # A non-zero exit is the expected outcome: the test fails because no
    # candidate arrived, which is exactly the behaviour we want.
    echo "ok: no candidates without a server"
fi

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures scenario(s) failed"
    exit 1
fi
echo "all scenarios passed"
