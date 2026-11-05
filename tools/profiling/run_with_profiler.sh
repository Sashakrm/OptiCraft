#!/usr/bin/env bash
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR=${OPTICRAFT_BUILD_DIR:-$ROOT/cmake-build-debug}
GAME_BIN=${OPTICRAFT_GAME_BIN:-$BUILD_DIR/OptiCraft}
PROFILER_BIN=${TRACY_PROFILER_BIN:-$ROOT/tools/profiling/tracy-profiler}

# Fall back to a profiler built outside the repository if the packaged binary
# was intentionally removed or a different Tracy build is needed.
if [ ! -x "$PROFILER_BIN" ] && [ -x "$BUILD_DIR/tracy-profiler/tracy-profiler" ]; then
    PROFILER_BIN=$BUILD_DIR/tracy-profiler/tracy-profiler
fi
if [ ! -x "$PROFILER_BIN" ] && [ -x /home/noktemor/tracy/profiler/build/tracy-profiler ]; then
    PROFILER_BIN=/home/noktemor/tracy/profiler/build/tracy-profiler
fi

[ -x "$GAME_BIN" ] || { echo "Не найдена игра: $GAME_BIN (соберите OptiCraft в CLion)" >&2; exit 1; }
[ -x "$PROFILER_BIN" ] || {
    echo "Не найден GUI-профайлер Tracy: $PROFILER_BIN" >&2
    echo "Задайте TRACY_PROFILER_BIN или соберите tracy-profiler отдельно." >&2
    exit 1
}

"$PROFILER_BIN" >"$BUILD_DIR/tracy-profiler.log" 2>&1 &
PROFILER_PID=$!

cleanup() {
    kill -TERM "$PROFILER_PID" 2>/dev/null || true
    wait "$PROFILER_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

cd "$BUILD_DIR" || exit 1
"$GAME_BIN"
