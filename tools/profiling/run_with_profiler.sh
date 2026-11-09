#!/usr/bin/env bash
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR=${OPTICRAFT_BUILD_DIR:-$ROOT/cmake-build-debug}
GAME_BIN=${OPTICRAFT_GAME_BIN:-$BUILD_DIR/OptiCraft}
PROFILER_BIN=${TRACY_PROFILER_BIN:-$BUILD_DIR/tracy-profiler/tracy-profiler}

# Prefer the build-tree profiler: it is compiled from the same Tracy sources as the game.
# The checked-in standalone binary may use a different protocol version.
if [ ! -x "$PROFILER_BIN" ] && [ -x "$ROOT/tools/profiling/tracy-profiler" ]; then
    PROFILER_BIN=$ROOT/tools/profiling/tracy-profiler
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

# Открыть профиль подключения сразу: без -a Tracy показывает список клиентов и ждёт ручного Connect.
"$PROFILER_BIN" -a 127.0.0.1 >"$BUILD_DIR/tracy-profiler.log" 2>&1 &
PROFILER_PID=$!

cleanup() {
    kill -TERM "$PROFILER_PID" 2>/dev/null || true
    wait "$PROFILER_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

cd "$BUILD_DIR" || exit 1
"$GAME_BIN"
