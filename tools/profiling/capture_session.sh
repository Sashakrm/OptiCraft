#!/bin/bash
# Сессия игры с записью Tracy. Запускает tracy-capture и игру; когда игра закрывается
# (или нажат Ctrl+C / Stop в CLion), сохраняет запись .tracy и сразу строит посекундную сводку
# для анализа (CSV + Markdown, можно отдать ИИ).
#
# usage: tools/profiling/capture_session.sh [путь/к/OptiCraft]
#   по умолчанию игра: cmake-build-debug/OptiCraft
#
# Переменные окружения (необязательные):
#   SESSION_MODE     creative (по умолчанию) или survival: мир грузится сразу, как в замерах
#   SESSION_SECONDS  остановить игру через N секунд (удобно для проверки)
#   OUT_DIR          куда писать результаты (по умолчанию profiling-results/session-ДАТА-ВРЕМЯ)
#   TRACY_TOOLS      папка с tracy-capture и tracy-csvexport (по умолчанию cmake-build-debug/tracy-tools;
#                    если утилит там нет — собираются автоматически из external/tracy)
#   DISPLAY          нужен X-сервер (в Linux без экрана: Xvfb :99 &)
#
# Требуются пакеты для сборки утилит Tracy (Debian/Ubuntu): libzstd-dev libcapstone-dev
set -u
set -o pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
GAME_BIN=${1:-$ROOT/cmake-build-debug}
GAME_BIN=$( [ -f "$GAME_BIN" ] && echo "$GAME_BIN" || echo "$GAME_BIN/OptiCraft" )
TOOLS=${TRACY_TOOLS:-$ROOT/cmake-build-debug/tracy-tools}
STAMP=$(date +%Y%m%d-%H%M%S)
OUT=${OUT_DIR:-$ROOT/profiling-results/session-$STAMP}
MODE=${SESSION_MODE:-creative}

[ -x "$GAME_BIN" ] || { echo "Не найдена игра: $GAME_BIN (сначала соберите проект в CLion)" >&2; exit 1; }
BIN_DIR=$(cd "$(dirname "$GAME_BIN")" && pwd)
mkdir -p "$OUT" "$TOOLS"

# --- утилиты Tracy: собираем при первом запуске ---------------------------------
build_tool() {  # $1 — имя утилиты (tracy-capture | tracy-csvexport), $2 — папка исходников
    if [ ! -x "$TOOLS/$1" ]; then
        echo "Собираю $1 (один раз)..."
        cmake -S "$ROOT/external/tracy/$2" -B "$TOOLS/build-$1" -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "$TOOLS/build-$1.log" \
          && cmake --build "$TOOLS/build-$1" -j --target "$1" 2>&1 | tee -a "$TOOLS/build-$1.log"
        if [ -x "$TOOLS/build-$1/$1" ]; then cp "$TOOLS/build-$1/$1" "$TOOLS/$1"; fi
    fi
    [ -x "$TOOLS/$1" ] || { echo "Не удалось собрать $1, см. $TOOLS/build-$1.log" >&2; exit 1; }
}
build_tool tracy-capture capture
build_tool tracy-csvexport csvexport

# --- папка запуска: игра ищет assets относительно текущей папки ------------------------
RUN="$OUT/run"
mkdir -p "$RUN"
[ -e "$RUN/assets" ] || ln -s "$BIN_DIR/assets" "$RUN/assets"
[ -n "${DISPLAY:-}" ] || { echo "Нет DISPLAY: запустите X-сервер (например, Xvfb :99 &) и задайте DISPLAY" >&2; exit 1; }

# --- запись и игра -----------------------------------------------------------------
"$TOOLS/tracy-capture" -o "$OUT/session.tracy" -f > "$OUT/capture.log" 2>&1 &
CAP=$!
sleep 1

cd "$RUN" || exit 1
OPTICRAFT_AUTOSTART=$MODE "$GAME_BIN" > "$OUT/game.log" 2>&1 &
GAME_PID=$!
echo "Запись Tracy идёт, игра запущена. Закройте игру (или Ctrl+C), чтобы сохранить сессию."

# Остановка: сначала мягко (SIGINT), если игра не вышла за 5 с — принудительно.
stop_game() {
    kill -INT "$GAME_PID" 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "$GAME_PID" 2>/dev/null || return
        sleep 0.5
    done
    kill -KILL "$GAME_PID" 2>/dev/null
}
trap stop_game INT TERM

if [ -n "${SESSION_SECONDS:-}" ]; then
    ( sleep "$SESSION_SECONDS"; stop_game ) &
    WATCHDOG=$!
fi

# wait прерывается обработчиком Ctrl+C, поэтому ждём в цикле, пока игра действительно не завершится
while kill -0 "$GAME_PID" 2>/dev/null; do wait "$GAME_PID" 2>/dev/null; done
[ -n "${WATCHDOG:-}" ] && kill "$WATCHDOG" 2>/dev/null

# --- сохранение записи ---------------------------------------------------------------
kill -INT "$CAP" 2>/dev/null
for _ in $(seq 1 20); do kill -0 "$CAP" 2>/dev/null || break; sleep 0.5; done
kill -KILL "$CAP" 2>/dev/null
wait "$CAP" 2>/dev/null

if [ ! -s "$OUT/session.tracy" ]; then
    echo "Запись не сохранилась: игра не подключилась к tracy-capture. См. $OUT/capture.log и $OUT/game.log" >&2
    exit 1
fi
echo "Игра закрыта, запись сохранена: $OUT/session.tracy"

# --- посекундная сводка -----------------------------------------------------------------
if python3 "$ROOT/tools/profiling/summarize_trace.py" "$OUT/session.tracy" \
        --csvexport "$TOOLS/tracy-csvexport" --out "$OUT"; then
    echo "Сводка: $OUT/session_summary.md и $OUT/session_per_second.csv"
else
    echo "Сводка не построена (см. выше). Запись сохранена: $OUT/session.tracy" >&2
fi
