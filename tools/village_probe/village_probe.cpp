// Проверка размещения деревень: сколько центров на области, и стоит ли маркер в мире.
#include "world/generation/World_Generator.h"
#include "world/generation/Structure_Placement.h"
#include "world/generation/River_Network.h"

#include "world/chunks/Chunk.h"

#include <cstdio>
#include <cstdlib>

inline int floordiv_v(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

int main(int argc, char** argv) {
    const uint32_t seed = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 1327u;
    const int cells = argc > 2 ? std::atoi(argv[2]) : 16;   // сторона области в ячейках
    World_Generator gen("Classic", seed, Dimension_Id::Overworld);
    River_Network rivers(seed, [&](int x, int z) { return gen.get_height(x, z); }, World_Generator::sea_level());
    Structure_Placement st(seed, [&](int x, int z) { return gen.get_height(x, z); }, &rivers, World_Generator::sea_level());

    int count = 0, marker_ok = 0, marker_bad = 0;
    const int half = cells / 2;
    for (int cz = -half; cz < half; ++cz)
        for (int cx = -half; cx < half; ++cx) {
            const auto site = st.village_in_cell(cx, cz);
            if (!site) continue;
            ++count;
            if (count <= 8)
                std::printf("деревня: центр (%d, %d), высота земли %d\n", site->x, site->z, gen.get_height(site->x, site->z));
            // Маркер: столб от земли до земли+100 должен быть, на 101-м блоке — воздух.
            const int h = gen.get_height(site->x, site->z);
            bool ok = gen.get_block(site->x, h + 1, site->z) == Block_Types::Diamond &&
                      gen.get_block(site->x, h + Structure_Placement::kMarkerHeight, site->z) == Block_Types::Diamond &&
                      gen.get_block(site->x, h + Structure_Placement::kMarkerHeight + 1, site->z) != Block_Types::Diamond;
            (ok ? marker_ok : marker_bad)++;
        }
    std::printf("Деревень на области %d x %d ячеек: %d (на км² ≈ %.2f)\n", cells, cells, count,
                count / ((cells * Structure_Placement::kCellSize / 1000.0) * (cells * Structure_Placement::kCellSize / 1000.0)));
    std::printf("Маркер в мире: OK %d, ошибок %d\n", marker_ok, marker_bad);

    // Чанки с маркером: generate_chunk должен совпасть с get_block (проверка solid_top и пустых чанков).
    int chunks = 0, chunk_mismatch = 0;
    for (int cz = -half; cz < half; ++cz)
        for (int cx = -half; cx < half; ++cx) {
            const auto site = st.village_in_cell(cx, cz);
            if (!site) continue;
            const int chx = floordiv_v(site->x, Config::chunk_size);
            const int chz = floordiv_v(site->z, Config::chunk_size);
            const int h = gen.get_height(site->x, site->z);
            for (int chy = floordiv_v(h, Config::chunk_height); chy <= floordiv_v(h + 101, Config::chunk_height); ++chy) {
                Chunk_Block_Grid grid;
                gen.generate_chunk(grid, chx, chy, chz);
                ++chunks;
                for (int z = 0; z < Config::chunk_size; ++z)
                    for (int y = 0; y < Config::chunk_height; ++y)
                        for (int x = 0; x < Config::chunk_size; ++x) {
                            const int wx = chx * Config::chunk_size + x;
                            const int wy = chy * Config::chunk_height + y;
                            const int wz = chz * Config::chunk_size + z;
                            if (grid.get(x, y, z) != gen.get_block(wx, wy, wz)) ++chunk_mismatch;
                        }
            }
        }
    std::printf("Чанки с деревнями: %d, расхождений generate_chunk/get_block: %d\n", chunks, chunk_mismatch);
    return 0;
}
