#pragma once

// Обёртка над Tracy-профайлером (https://github.com/wolfpld/tracy).
//
//   OC_ZONE("имя")          — замер области от этой строки до конца текущего блока {}.
//   OC_FRAME_MARK            — отметка конца кадра (для счётчика FPS в Tracy).
//   OC_PLOT("имя", value)    — числовой график (например, число грязных чанков).
//
// Если папка external/tracy отсутствует или проект собран с -DOPTICRAFT_TRACY=OFF,
// все макросы превращаются в пустые выражения и не стоят ничего.
// Сам клиент включается определением TRACY_ENABLE (см. CMakeLists.txt).
//
// oc_profiling::loop_frame() — номер кадра главного цикла (растёт в OC_FRAME_MARK). Он совпадает с
// порядковым номером зоны "Frame" в записи Tracy, поэтому строки "[GPUSTAT]" из game.log
// (см. Renderer::poll_gpu_phase_timers) привязываются к секундам отчёта точно, без сдвига часов.

#include <atomic>
#include <cstdint>

namespace oc_profiling {
inline uint64_t& loop_frame() {
    static uint64_t counter = 0;
    return counter;
}

// Счётчики пересборок меша и копий граней соседей. Растут на главном потоке при постановке задач мешера
// (Chunk_Manager::enqueue_mesh_rebuild); Renderer печатает их в строке [GPUSTAT] раз в секунду.
// Причины: geometry — блоки менялись; light_baked — свет менял квады (дальний LOD или откат); light_only —
// пересобран только световой объём (геометрия не трогалась); other — первый меш, смена LOD, срочная правка.
struct Remesh_Counters {
    std::atomic<uint64_t> geometry{0};
    std::atomic<uint64_t> light_baked{0};
    std::atomic<uint64_t> light_only{0};
    std::atomic<uint64_t> other{0};
    std::atomic<uint64_t> face_copies{0};
};
inline Remesh_Counters& remesh_counters() {
    static Remesh_Counters counters;
    return counters;
}
}

#if __has_include("tracy/Tracy.hpp")
#include "tracy/Tracy.hpp"
#define OC_ZONE(name) ZoneScopedN(name)
#define OC_FRAME_MARK do { ++::oc_profiling::loop_frame(); FrameMark; } while (0)
#define OC_PLOT(name, value) TracyPlot(name, static_cast<double>(value))
#else
#define OC_ZONE(name) ((void)0)
#define OC_FRAME_MARK do { ++::oc_profiling::loop_frame(); } while (0)
#define OC_PLOT(name, value) ((void)0)
#endif
