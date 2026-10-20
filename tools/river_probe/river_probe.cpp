// Проверка рек: карта, стыки регионов и детерминизм. Сборка: ./build.sh
#include "world/generation/World_Generator.h"
#include "world/generation/River_Network.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    const uint32_t seed = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 1327u;
    const int side = argc > 2 ? std::atoi(argv[2]) : 1024;
    const int ox = -side / 2, oz = -side / 2;

    World_Generator gen("Classic", seed, Dimension_Id::Overworld);
    River_Network rivers(seed, [&](int x, int z) { return gen.get_height(x, z); }, World_Generator::sea_level());

    if (std::getenv("RIVER_HCHECK")) {  // проверка детерминизма высот между генераторами
        World_Generator g2("Classic", seed, Dimension_Id::Overworld);
        int diff = 0;
        for (int z = -512; z < 512; z += 7) for (int x = -600; x < 600; x += 5) {
            int a = gen.get_height(x, z); int b = g2.get_height(x, z); if (a != b) ++diff;
        }
        std::printf("get_height расхождения между генераторами: %d\n", diff);
    }
    // 1) Карта: вода — синий (темнее глубже), водопад — белый, суша — зелёная по высоте.
    std::vector<unsigned char> img(static_cast<size_t>(side) * side * 3);
    size_t river_cols = 0, fall_cols = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            const River_Network::Column c = rivers.column_at(ox + x, oz + z);
            unsigned char r, g, b;
            if (c.river) {
                ++river_cols;
                if (c.fall != River_Network::kNoFall) {
                    ++fall_cols;
                    r = g = b = 240;
                } else {
                    const int depth = std::min(255, (c.water - c.bed) * 40);
                    r = 20; g = 60 + depth / 4; b = 150 + depth / 4;
                }
            } else {
                const int h = std::max(0, std::min(255, (c.top - 50) * 3));
                r = static_cast<unsigned char>(40 + h / 3);
                g = static_cast<unsigned char>(90 + h / 2);
                b = 40;
            }
            const size_t p = (static_cast<size_t>(z) * side + x) * 3;
            img[p] = r; img[p + 1] = g; img[p + 2] = b;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    std::printf("Колонки рек: %zu из %d, водопадов: %zu\n", river_cols, side * side, fall_cols);
    std::printf("Время на %d колонок (с регионами): %.1f мс\n", side * side,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
    stbi_write_png("river_projection.png", side, side, 3, img.data(), side * 3);

    // 2) Стыки регионов: на границе X = k*256 уровень воды соседних колонок должен почти совпадать.
    int seam_pairs = 0, seam_max_jump = 0;
    for (int bx = ox; bx < ox + side; ++bx) {
        if (((bx % River_Network::kRegion) + River_Network::kRegion) % River_Network::kRegion != 0) continue;
        for (int z = oz; z < oz + side; ++z) {
            const auto a = rivers.column_at(bx - 1, z);
            const auto b = rivers.column_at(bx, z);
            // Граница русла между соседними колонками — норма; сравниваем только пары речных колонок.
            if (a.river != b.river) continue;
            if (a.river) {
                ++seam_pairs;
                seam_max_jump = std::max(seam_max_jump, std::abs(a.water - b.water));
            }
        }
    }
    std::printf("Стыки X: пар речных колонок %d, макс. скачок уровня воды %d блок(ов)\n",
                seam_pairs, seam_max_jump);

    // 3) Детерминизм: после сброса кэша значения те же.
    bool same = true;
    std::vector<River_Network::Column> first;
    for (int z = oz; z < oz + 256; z += 3)
        for (int x = ox; x < ox + 256; x += 3)
            first.push_back(rivers.column_at(x, z));
    rivers.clear_cache();
    size_t k = 0;
    for (int z = oz; z < oz + 256; z += 3)
        for (int x = ox; x < ox + 256; x += 3) {
            const auto c = rivers.column_at(x, z);
            if (c.river != first[k].river || c.water != first[k].water || c.bed != first[k].bed ||
                c.fall != first[k].fall || c.top != first[k].top)
                same = false;
            ++k;
        }
    std::printf("Детерминизм после сброса кэша: %s\n", same ? "OK" : "РАСХОЖДЕНИЕ");
    return 0;
}
