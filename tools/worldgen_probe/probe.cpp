// Проверка генератора пещер без OpenGL: сверяет generate_chunk с get_block, проверяет правила
// (ничего ниже -60 вне закрытых пещер, входы на поверхность, вода) и независимость от порядка
// генерации и границ регионов. Собирается ./build.sh; см. комментарии внутри.
#include "world/generation/World_Generator.h"
#include "world/generation/Cave_Network.h"
#include "world/chunks/Chunk.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Stats {
    long voxels = 0;
    long mismatch_get_block = 0;     // generate_chunk != get_block
    long air_below_floor = 0;        // воздух ниже -60, которого нет ни в одной пещере
    long cave_but_solid = 0;         // cave_at=true, а блок твёрдый
    long openings = 0;               // столбцы, где пещера касается поверхности
    long water_cave = 0;             // вода внутри пещер
};

Stats g_stats;

void check_chunk(World_Generator& gen, int cx, int cy, int cz, Chunk_Block_Grid& grid) {
    for (int z = 0; z < Config::chunk_size; ++z)
        for (int y = 0; y < Config::chunk_height; ++y)
            for (int x = 0; x < Config::chunk_size; ++x) {
                const int wx = cx * Config::chunk_size + x;
                const int wy = cy * Config::chunk_height + y;
                const int wz = cz * Config::chunk_size + z;
                const Block_Types b = grid.get(x, y, z);
                const Block_Types ref = gen.get_block(wx, wy, wz);
                ++g_stats.voxels;
                if (b != ref) ++g_stats.mismatch_get_block;
                const bool is_air = b == Block_Types::Air;
                if (is_air && wy < -60 && !gen.cave_at(wx, wy, wz)) ++g_stats.air_below_floor;
                if (gen.cave_at(wx, wy, wz) && !is_air && b != Block_Types::Water) ++g_stats.cave_but_solid;
                if (gen.cave_at(wx, wy, wz) && b == Block_Types::Water) ++g_stats.water_cave;
            }
}

// Цепочка пересекающих поверхность чанков: считаем столбцы-входы по get_block.
void count_openings(World_Generator& gen, int x0, int x1, int z0, int z1) {
    for (int z = z0; z < z1; ++z)
        for (int x = x0; x < x1; ++x) {
            const int h = gen.get_height(x, z);
            if (h < -60 || h > 200) continue;
            if (gen.cave_at(x, h, z)) ++g_stats.openings;
        }
}

bool same_chunk(World_Generator& a, World_Generator& b, int cx, int cy, int cz) {
    Chunk_Block_Grid ga, gb;
    a.generate_chunk(ga, cx, cy, cz);
    b.generate_chunk(gb, cx, cy, cz);
    for (int y = 0; y < Config::chunk_height; ++y)
        for (int z = 0; z < Config::chunk_size; ++z)
            for (int x = 0; x < Config::chunk_size; ++x)
                if (ga.get(x, y, z) != gb.get(x, y, z)) return false;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const uint32_t seed = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 1327u;
    const int radius_chunks = argc > 2 ? std::atoi(argv[2]) : 8;   // половина стороны области, чанков

    World_Generator gen("Classic", seed, Dimension_Id::Overworld);

    // 1) Правила и согласованность generate_chunk с get_block.
    const auto t0 = std::chrono::steady_clock::now();
    int chunks = 0;
    for (int cz = -radius_chunks; cz < radius_chunks; ++cz)
        for (int cx = -radius_chunks; cx < radius_chunks; ++cx)
            for (int cy = -8; cy <= 3; ++cy) {
                Chunk_Block_Grid grid;
                gen.generate_chunk(grid, cx, cy, cz);
                check_chunk(gen, cx, cy, cz, grid);
                ++chunks;
            }
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    count_openings(gen, -radius_chunks * 32, radius_chunks * 32, -radius_chunks * 32, radius_chunks * 32);

    // Чистое время generate_chunk без проверок (то, что платит загрузчик чанков).
    {
        World_Generator timing_gen("Classic", seed, Dimension_Id::Overworld);
        const auto a = std::chrono::steady_clock::now();
        for (int cz = -radius_chunks; cz < radius_chunks; ++cz)
            for (int cx = -radius_chunks; cx < radius_chunks; ++cx)
                for (int cy = -8; cy <= 3; ++cy) {
                    Chunk_Block_Grid grid;
                    timing_gen.generate_chunk(grid, cx, cy, cz);
                }
        const auto b = std::chrono::steady_clock::now();
        const double pure = std::chrono::duration<double>(b - a).count();
        std::printf("pure generate_chunk: %.2f ms/chunk (%d chunks)\n", 1000.0 * pure / chunks, chunks);
    }

    // Закрытые пещеры и входы в воду (столбцы, где поверхность ниже уровня моря, а пещера ее касается).
    long sealed_vox = 0, water_openings = 0;
    for (int z = -radius_chunks * 32; z < radius_chunks * 32; ++z)
        for (int x = -radius_chunks * 32; x < radius_chunks * 32; ++x) {
            const int h = gen.get_height(x, z);
            if (h < 63 && gen.cave_at(x, h, z)) ++water_openings;
            for (int y = -150; y <= -63; ++y) if (gen.cave_at(x, y, z)) ++sealed_vox;
        }
    std::printf("sealed cave voxels (-150..-63): %ld\n", sealed_vox);

    // Проекция сверху: серый рельеф, сеть пещер — тёмно-синяя/жёлтая по глубине, входы — белые.
    {
        const int side = 2 * radius_chunks * 32;
        std::vector<uint8_t> img(static_cast<size_t>(side) * side * 3, 0);
        for (int z = 0; z < side; ++z)
            for (int x = 0; x < side; ++x) {
                const int wx = x - radius_chunks * 32, wz = z - radius_chunks * 32;
                const int h = gen.get_height(wx, wz);
                uint8_t r = static_cast<uint8_t>(std::clamp(h, 0, 255) / 2 + 40);
                uint8_t g = r, b = r;
                if (h < 63) { r = 30; g = 60; b = 120; }
                for (int y = h; y >= -150; --y) {
                    if (!gen.cave_at(wx, y, wz)) continue;
                    if (y == h) { r = 255; g = 255; b = 255; }
                    else if (y >= -60) { r = 200; g = 170; b = 60; }
                    else { r = 120; g = 40; b = 140; }
                    break;
                }
                const size_t p = (static_cast<size_t>(z) * side + x) * 3;
                img[p] = r; img[p + 1] = g; img[p + 2] = b;
            }
        const std::string path = "probe_projection.png";
        stbi_write_png(path.c_str(), side, side, 3, img.data(), side * 3);
        std::printf("wrote %s\n", path.c_str());
    }
    std::printf("underwater openings (surface below sea level, cave at surface): %ld\n", water_openings);

    std::printf("seed=%u area=%dx%d chunks (chunk_y -8..3), chunks=%d, time=%.2fs (%.1f ms/chunk)\n",
                seed, 2 * radius_chunks * 32, 2 * radius_chunks * 32, chunks, secs, 1000.0 * secs / chunks);
    std::printf("voxels=%ld\n", g_stats.voxels);
    std::printf("generate_chunk vs get_block mismatches: %ld (must be 0)\n", g_stats.mismatch_get_block);
    std::printf("air below -60 outside caves: %ld (must be 0)\n", g_stats.air_below_floor);
    std::printf("cave voxels that are solid: %ld (must be 0)\n", g_stats.cave_but_solid);
    std::printf("water inside caves: %ld\n", g_stats.water_cave);
    std::printf("columns with cave at surface (openings): %ld\n", g_stats.openings);

    // 2) Независимость от порядка и границ регионов: один и тот же чанк в двух генераторах,
    //    один из которых сначала генерирует соседей. Чанки 7/8 — граница региона 256 блоков.
    World_Generator g1("Classic", seed, Dimension_Id::Overworld);
    World_Generator g2("Classic", seed, Dimension_Id::Overworld);
    bool ok = true;
    for (int cy = -2; cy <= 1; ++cy)
        for (int cz = -2; cz <= 2; ++cz)
            for (int cx = 6; cx <= 9; ++cx) {
                // g1: сначала соседний чанк, потом целевой; g2: сразу целевой.
                Chunk_Block_Grid throwaway;
                g1.generate_chunk(throwaway, cx - 1, cy, cz);
                ok = same_chunk(g1, g2, cx, cy, cz) && ok;
            }
    std::printf("order/region independence (chunks across x=256): %s\n", ok ? "OK" : "MISMATCH");
    return g_stats.mismatch_get_block == 0 && g_stats.air_below_floor == 0 &&
           g_stats.cave_but_solid == 0 && ok ? 0 : 1;
}
