#!/bin/bash
# Прогон OptiCraft под Tracy без GUI: игра стартует сама (OPTICRAFT_AUTOSTART), команды подаются
# по таймлайну через OPTICRAFT_CMDFILE, запись сохраняется в <OUT_DIR>/<имя>/<имя>.tracy.
#
# usage: tools/profiling/run_scenario.sh <имя> <каталог_сборки> <creative|survival> <таймлайн.tl>
#
# Переменные окружения:
#   TRACY_CAPTURE  путь к tracy-capture (по умолчанию: tracy-capture из PATH)
#   OUT_DIR        куда писать результаты (по умолчанию: ./profiling-results)
#   DISPLAY        уже запущенный X-сервер; если не задан, поднимается Xvfb на :99
#
# Таймлайн: строки "<пауза_сек> <команда_без_слэша>". Игра забирает команду, когда мир загружен,
# поэтому первая строка фактически ждёт окончания загрузки. Последняя строка задаёт длительность записи.
set -u
if [ $# -ne 4 ]; then
  echo "Нужно 4 аргумента: <имя> <каталог_сборки> <creative|survival> <таймлайн.tl>" >&2
  echo "Пример: tools/profiling/run_scenario.sh travel cmake-build-debug creative tools/profiling/travel.tl" >&2
  echo "Для одной сессии без таймлайна проще: tools/profiling/capture_session.sh" >&2
  exit 1
fi
NAME=$1; BIN=$2; MODE=$3; TL=$4
[ -d "$BIN" ] || { echo "Каталог сборки не найден: $BIN" >&2; exit 1; }
[ -f "$TL" ] || { echo "Таймлайн не найден: $TL" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
OUT_ROOT=${OUT_DIR:-$PWD/profiling-results}
CAP=${TRACY_CAPTURE:-$(command -v tracy-capture || true)}
[ -x "$CAP" ] || { echo "tracy-capture не найден: собери external/tracy/capture и задай TRACY_CAPTURE" >&2; exit 1; }
OUT="$OUT_ROOT/$NAME"; RUN="$OUT/run"
rm -rf "$OUT" && mkdir -p "$RUN"
ln -s "$(cd "$BIN" && pwd)/assets" "$RUN/assets"
cd "$RUN" || exit 1
if [ -z "${DISPLAY:-}" ]; then
  export DISPLAY=:99
  (Xvfb :99 -screen 0 1280x720x24 -nolisten tcp > "$OUT/xvfb.log" 2>&1 &)
  sleep 2
fi
( "$CAP" -o "$OUT/$NAME.tracy" -f > "$OUT/capture.log" 2>&1 & echo $! > "$OUT/capture.pid" )
sleep 1
CMDFILE="$RUN/cmd.txt"
OPTICRAFT_AUTOSTART=$MODE OPTICRAFT_CMDFILE="$CMDFILE" "$BIN/OptiCraft" > "$OUT/game.log" 2>&1 &
GAME=$!
while read -r delay cmd; do
  [ -z "$delay" ] && continue
  sleep "$delay"
  printf '%s\n' "$cmd" > "$CMDFILE.tmp" && mv "$CMDFILE.tmp" "$CMDFILE"
  echo "$(date +%T) queued: $cmd" >> "$OUT/timeline.log"
  while [ -f "$CMDFILE" ]; do sleep 0.2; done
  echo "$(date +%T) consumed: $cmd" >> "$OUT/timeline.log"
done < "$TL"
kill -INT $GAME 2>/dev/null; sleep 4; kill -KILL $GAME 2>/dev/null
kill -INT "$(cat "$OUT/capture.pid")" 2>/dev/null; sleep 5
echo "Готово: $OUT/$NAME.tracy"
