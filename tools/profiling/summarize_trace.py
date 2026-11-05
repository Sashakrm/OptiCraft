#!/usr/bin/env python3
"""Посекундная сводка по записи Tracy (.tracy) для анализа нагрузок.

Использование:
    python3 tools/profiling/summarize_trace.py <файл.tracy> [--csvexport путь/к/tracy-csvexport] [--out папка]

Результат (в папке --out, по умолчанию рядом с .tracy):
    <имя>_per_second.csv   — таблица по секундам, по одной строке на секунду сессии
    <имя>_summary.md       — краткая сводка, таблица по секундам, список пиков кадров

Как читать колонки:
    Все «_ms» — суммарное время зон, НАЧАВШИХСЯ в эту секунду.
    Главный поток (кадры, логика, рендер) — его время не перекрывается, сумма обычно <= 1000 мс.
    Колонки gen_* — рабочие потоки, там сумма по всем потокам, может быть больше 1000 мс.
Скрипт не зависит от сторонних пакетов Python.
"""
import argparse
import collections
import csv
import pathlib
import subprocess
import sys

# Колонка -> точное имя зоны в Tracy (см. documentation/PROFILING.md, раздел 2).
MAIN_ZONES = {
    "render_ms": "App::render (state render)",
    "update_ms": "App::update (state update)",
    "input_ms": "App::handle_input",
    "present_ms": "Frame tail: GPU present (swap)",
    "mesher_ms": "submit: GPU mesher dispatch (greedy+misc)",
    "draw_ms": "Renderer::draw_gpu_kind",
    "hiz_ms": "Renderer::build_gpu_hiz",
    "player_pos_ms": "ChunkMgr::update_player_position",
    "enqueue_mesh_ms": "ChunkMgr::enqueue_mesh_rebuild",
    "collect_chunks_ms": "ChunkMgr::collect_finished_chunks",
    "light_scan_ms": "apply: light/liquid scan of new chunk",
    "sky_light_ms": "ChunkMgr::recompute_sky_lighting",
    "fluid_ms": "Fluid::update",
    "mobs_ms": "Mobs::update_frame",
}
WORKER_ZONES = {
    "gen_ms": "Gen::generate_prepared_chunk_impl",
}
COUNT_ZONES = {
    "chunks_applied": "apply: insert chunk into table",
    "gen_chunks": "Gen::generate_prepared_chunk_impl",
}
FRAME_ZONE = "Frame"
TOP_LEVEL = ("App::render (state render)", "App::update (state update)",
             "App::handle_input", "Frame tail: GPU present (swap)", "Frame tail: window events (poll)")
SPIKE_MS = 100.0


def read_events(csvexport, trace):
    proc = subprocess.Popen([csvexport, "-u", str(trace)], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace")
    reader = csv.reader(proc.stdout)
    header = next(reader, None)
    if header is None:  # csvexport ничего не выдал: пустая или повреждённая запись
        proc.wait()
        return
    idx = {k: header.index(k) for k in ("name", "ns_since_start", "exec_time_ns", "thread")}
    for row in reader:
        if len(row) < len(header):
            continue  # защита от экзотических имён с запятыми
        yield (row[idx["name"]], int(row[idx["ns_since_start"]]),
               int(row[idx["exec_time_ns"]]), row[idx["thread"]])
    proc.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", type=pathlib.Path)
    ap.add_argument("--csvexport", default="tracy-csvexport")
    ap.add_argument("--out", type=pathlib.Path, default=None)
    args = ap.parse_args()

    trace = args.trace.resolve()
    out_dir = (args.out or trace.parent).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = trace.stem

    # Первый проход: определяем главный поток (тот, где идут зоны Frame).
    frame_threads = collections.Counter()
    events_main_top = []   # (start, dur, name) верхнеуровневые зоны главного потока — для пиков
    frames = []            # (start, dur)
    per_sec = collections.defaultdict(lambda: collections.defaultdict(float))
    counts = collections.defaultdict(lambda: collections.defaultdict(int))
    main_thread = None
    first_ns = None
    last_ns = 0

    all_names = set(MAIN_ZONES.values()) | set(WORKER_ZONES.values()) | {FRAME_ZONE}
    # Два прохода не нужны: сначала узнаём главный поток по зоне Frame из потока-кандидата.
    buffered = []
    for name, start, dur, thread in read_events(args.csvexport, trace):
        if name == FRAME_ZONE:
            frame_threads[thread] += 1
        if first_ns is None or start < first_ns:
            first_ns = start
        last_ns = max(last_ns, start + dur)
        if name in all_names or name in TOP_LEVEL:
            buffered.append((name, start, dur, thread))
    if not frame_threads:
        sys.exit("В записи нет зон Frame — нечего суммировать (игра запускалась с Tracy?)")
    main_thread = frame_threads.most_common(1)[0][0]
    base = first_ns or 0

    for name, start, dur, thread in buffered:
        sec = (start - base) // 1_000_000_000
        ms = dur / 1e6
        if name == FRAME_ZONE and thread == main_thread:
            frames.append((start, dur))
            per_sec[sec]["frames"] += 1
            per_sec[sec]["frame_sum_ms"] += ms
            per_sec[sec]["frame_max_ms"] = max(per_sec[sec]["frame_max_ms"], ms)
            continue
        if thread == main_thread and name in TOP_LEVEL:
            events_main_top.append((start, dur, name))
        for col, zone in MAIN_ZONES.items():
            if name == zone and thread == main_thread:
                per_sec[sec][col] += ms
        for col, zone in WORKER_ZONES.items():
            if name == zone:
                per_sec[sec][col] += ms
        for col, zone in COUNT_ZONES.items():
            if name == zone:
                counts[sec][col] += 1

    n_sec = (last_ns - base) // 1_000_000_000 + 1
    cols = ["t_s", "frames", "frame_avg_ms", "frame_max_ms", "render_ms", "update_ms", "input_ms",
            "present_ms", "mesher_ms", "draw_ms", "hiz_ms", "player_pos_ms", "enqueue_mesh_ms",
            "collect_chunks_ms", "light_scan_ms", "sky_light_ms", "fluid_ms", "mobs_ms",
            "chunks_applied", "gen_chunks", "gen_ms"]
    rows = []
    for s in range(int(n_sec)):
        d = per_sec.get(s, {})
        c = counts.get(s, {})
        n = int(d.get("frames", 0))
        row = {"t_s": s, "frames": n,
               "frame_avg_ms": round(d.get("frame_sum_ms", 0) / n, 1) if n else 0,
               "frame_max_ms": round(d.get("frame_max_ms", 0), 1)}
        for col in MAIN_ZONES:
            row[col] = round(d.get(col, 0), 1)
        row["chunks_applied"] = c.get("chunks_applied", 0)
        row["gen_chunks"] = c.get("gen_chunks", 0)
        row["gen_ms"] = round(d.get("gen_ms", 0), 1)
        rows.append(row)

    csv_path = out_dir / f"{stem}_per_second.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)

    # Сводка по всей сессии.
    frame_ms = sorted(d / 1e6 for _, d in frames)
    total_s = max(1e-9, (last_ns - base) / 1e9)
    lines = [f"# Сводка профиля: {stem}", "",
             f"- Длительность записи: {total_s:.1f} с",
             f"- Кадров: {len(frame_ms)}, средний кадр: {sum(frame_ms)/max(1,len(frame_ms)):.1f} мс "
             f"(≈{1000*len(frame_ms)/max(1e-9,sum(frame_ms)):.1f} FPS по времени кадров)",
             f"- Медиана кадра: {frame_ms[len(frame_ms)//2]:.1f} мс, p95: {frame_ms[int(len(frame_ms)*0.95)]:.1f} мс, "
             f"максимум: {frame_ms[-1]:.1f} мс" if frame_ms else "- Кадров не найдено",
             f"- Кадров длиннее {SPIKE_MS:.0f} мс: {sum(1 for x in frame_ms if x > SPIKE_MS)}", ""]

    # Пики: для каждого длинного кадра — какая верхнеуровневая часть заняла больше всего времени.
    spikes = [(s, d) for s, d in frames if d / 1e6 > SPIKE_MS]
    if spikes:
        top_parts = sorted(events_main_top)
        lines += ["## Пики кадров", "", "| t, с | кадр, мс | главная часть | её время, мс |",
                  "|---:|---:|---|---:|"]
        for s, d in sorted(spikes, key=lambda x: -x[1])[:20]:
            t0, t1 = s, s + d
            inside = [(dur, name) for st, dur, name in top_parts if st >= t0 and st + dur <= t1]
            if inside:
                dur, name = max(inside)
                part, part_ms = name, dur / 1e6
            else:
                part, part_ms = "—", 0.0
            lines.append(f"| {(s-base)/1e9:.2f} | {d/1e6:.1f} | {part} | {part_ms:.1f} |")
        lines.append("")

    lines += ["## Посекундная таблица", "",
              "Времена — суммарно по зонам, начавшимся в эту секунду. Колонки gen_* — рабочие потоки.", "",
              "| " + " | ".join(cols) + " |", "|" + "---:|" * len(cols)]
    for r in rows:
        lines.append("| " + " | ".join(str(r[c]) for c in cols) + " |")
    lines += ["", "## Легенда зон", ""]
    for col, zone in {**MAIN_ZONES, **WORKER_ZONES}.items():
        lines.append(f"- `{col}` — `{zone}`")
    md_path = out_dir / f"{stem}_summary.md"
    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"Готово:\n  {csv_path}\n  {md_path}")


if __name__ == "__main__":
    main()
