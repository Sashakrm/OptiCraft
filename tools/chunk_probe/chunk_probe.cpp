// Проверка планировщика Chunk_Manager без GL: летим по прямой с заданной скоростью и считаем,
// сколько чанков загружено, какое вертикальное окно и как быстро растёт очередь.
#include "world/chunks/Chunk_Manager.h"
#include "world/generation/World_Generator.h"
#include "rendering/atlas/Texture_Atlas.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

int main(int argc, char** argv) {
    const int radius = argc > 1 ? std::atoi(argv[1]) : 16;
    const float vy = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 0.0f;     // блоков/с по Y
    const float vx = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 0.0f;     // блоков/с по X
    const int frames = argc > 4 ? std::atoi(argv[4]) : 300;

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "chunk_probe_world";
    std::filesystem::create_directories(dir);

    World_Generator gen("Classic", 1327u, Dimension_Id::Overworld);
    World_File file;
    Chunk_Manager cm(radius);
    cm.set_world_generator(gen);
    cm.set_world_storage(file, "Classic");
    static Texture_Atlas dummy_atlas;   // без GL: пустой атлас, UV не нужны для счёта чанков
    cm.set_texture_atlas(dummy_atlas);

    float x = 0.0f, y = 90.0f, z = 0.0f;
    const float dt = 1.0f / 60.0f;
    auto start = std::chrono::steady_clock::now();
    size_t max_loaded = 0, max_inflight_proxy = 0;
    for (int f = 0; f < frames; ++f) {
        x += vx * dt; y += vy * dt;
        cm.update_player_position(x, y, z);
        cm.collect_finished_chunks();
        max_loaded = std::max(max_loaded, cm.get_loaded_count());
        if (f % 60 == 59) {
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("  t=%.1f с, кадр %d: загружено %zu, y=%.0f\n", t, f + 1, cm.get_loaded_count(), y);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));   // реальный кадр ~60 FPS
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("радиус %d, скорость (%.0f, %.0f) блоков/с, кадров %d, %.2f с\n", radius, vx, vy, frames, secs);
    std::printf("загружено в конце: %zu, максимум: %zu\n", cm.get_loaded_count(), max_loaded);
    std::printf("ожидаемо на стоянке: %d^2 * 8 = %d\n", 2 * radius + 1, (2 * radius + 1) * (2 * radius + 1) * 8);
    (void)max_inflight_proxy;
    return 0;
}
