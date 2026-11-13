#pragma once


namespace Config {
    // Настройки графики по умолчанию (игрок меняет их в меню «Настройки»).
    // Длительность появления чанка, сек: диапазон для ползунка и значение по умолчанию.
    constexpr float chunk_fade_seconds_min = 0.10f;
    constexpr float chunk_fade_seconds_max = 0.50f;
    constexpr float chunk_fade_seconds_default = 0.42f;
    // Виньетка: уровни 0 (выкл), 1 (слабая), 2 (средняя, как раньше), 3 (сильная).
    constexpr int vignette_level_count = 4;
    constexpr int vignette_level_default = 2;
    // Сглаживание: 0 — выкл, 1 — FXAA.
    constexpr int antialiasing_mode_count = 2;
    // Зерно: доля от 0 до 1 на ползунке; амплитуда на экране = доля × film_grain_max_amplitude.
    constexpr float film_grain_max_amplitude = 0.08f;

    // Максимальный уровень света от блока (Sun Stone и т.п.), 0 = нет света. Свет затухает
    // на 1 за каждый блок пути через прозрачные соседние клетки (см. Chunk_Manager::propagate_light).
    // 14 подобрано под явное требование "радиус около 14 блоков".
    // Рендер
    constexpr float clear_color_r = 0.45f;
    constexpr float clear_color_g = 0.68f;
    constexpr float clear_color_b = 1.0f;
    constexpr bool vsync_enabled = false;

    // GPU-мешер чанков делит видеокарту с отрисовкой. Раньше его ограничивало только число чанков на кадр
    // (до 64) и 8 батчей в полёте — при 300 FPS это сотни чанков в секунду, и на встроенной графике
    // очередь GPU забивалась на секунды (кадры по 100-700 мс в swap_buffers). Теперь батч подбирается
    // под целевое время GPU, измеренное по glQueryCounter, а в полёте держится не больше двух батчей.
    constexpr float gpu_mesher_target_ms = 2.0f;           // цель: ~2 мс GPU на один батч мешера
    constexpr int gpu_mesher_max_batches_in_flight = 2;    // сколько батчей может ждать выполнения на GPU
    constexpr int gpu_mesher_first_batch_chunks = 4;       // размер батча, пока нет ни одного замера ms/чанк
    // Световой объём: свет и AO ближних чанков берутся на пикселе из объёма (mesh_volume.comp), квады не
    // зависят от света, а смена света пересобирает только объём. false — прежний путь: свет и AO запекаются
    // в квады, в ключ слияния, и смена света пересобирает геометрию (откат на случай проблем).
    constexpr bool gpu_light_volume = true;

}
