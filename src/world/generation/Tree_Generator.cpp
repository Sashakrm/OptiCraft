#include "world/generation/Tree_Generator.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
    struct Placed {
        int x, y, z;
        Block_Types type;
    };

    // Шар листвы с рваным краем: часть граничных блоков пропускается, а низ кроны подрезан,
    // поэтому крона выглядит как куст из нескольких шапок, а не как диск.
    void leaf_blob(std::vector<Placed>& out, int cx, int cy, int cz, int r,
                   Fast_Rng& rng, Block_Types leaves) {
        std::uniform_int_distribution<int> pct(0, 99);
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                for (int dz = -r; dz <= r; ++dz) {
                    const int d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 > r * r) continue;
                    const bool edge = d2 > (r - 1) * (r - 1);
                    if (edge && pct(rng) < 35) continue;
                    if (edge && dy == -r) continue;
                    out.push_back({cx + dx, cy + dy, cz + dz, leaves});
                }
            }
        }
    }

    void log_line(int x, int y, int z, int dx, int dz, int length,
                  Tree_Generator::Block_Setter setter, Block_Types log) {
        for (int i = 0; i < length; ++i) {
            setter(x + dx * i, y + i / 3, z + dz * i, log);
        }
    }

    void disk(Tree_Generator::Block_Setter setter, int cx, int cy, int cz,
              int radius, Block_Types leaves, bool hollow = false) {
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                const int d2 = dx * dx + dz * dz;
                if (d2 > radius * radius + 1) continue;
                if (hollow && d2 < std::max(0, radius * radius - 2)) continue;
                setter(cx + dx, cy, cz + dz, leaves);
            }
        }
    }

    void branch(Tree_Generator::Block_Setter setter, int x, int y, int z,
                int dx, int dz, int length, Block_Types log) {
        for (int i = 1; i <= length; ++i) {
            const int lift = (i + 1) / 3;
            setter(x + dx * i, y + lift, z + dz * i, log);
        }
    }
}

void Tree_Generator::place_tree(Tree_Type type, int x, int y, int z,
                                Block_Setter setter, Fast_Rng& rng) {
    switch (type) {
        case Tree_Type::Oak:       place_oak_tree(x, y, z, setter, rng); break;
        case Tree_Type::Palm:      place_palm_tree(x, y, z, setter, rng); break;
        case Tree_Type::Acacia:    place_acacia_tree(x, y, z, setter, rng); break;
        case Tree_Type::Birch:     place_birch_tree(x, y, z, setter, rng); break;
        case Tree_Type::Spruce:    place_spruce_tree(x, y, z, setter, rng); break;
        case Tree_Type::Jungle:    place_jungle_tree(x, y, z, setter, rng); break;
        case Tree_Type::Slimewood: place_slimewood_tree(x, y, z, setter, rng); break;
    }
}

// Дуб: прямой ствол, три ветки из верхней части в соседние стороны и несколько шаров листвы
// с рваным краем вокруг конца каждой ветки и вокруг верхушки. Крона начинается высоко, поэтому
// под ней можно пройти. Листва пишется раньше брёвен: там, где они пересекаются, побеждает ствол.
void Tree_Generator::place_oak_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hdist(6, 9);
    std::uniform_int_distribution<int> dir_dist(0, 3);
    std::uniform_int_distribution<int> len_dist(2, 3);
    std::uniform_int_distribution<int> rad_dist(2, 3);
    std::uniform_int_distribution<int> off_dist(-1, 1);
    const int h = hdist(rng);

    constexpr int kDx[4] = {1, 0, -1, 0};
    constexpr int kDz[4] = {0, 1, 0, -1};

    std::vector<Placed> logs;
    std::vector<Placed> leaves;
    for (int i = 1; i <= h; ++i) logs.push_back({x, y + i, z, Block_Types::Oak_Log});

    const int first = dir_dist(rng);
    for (int b = 0; b < 3; ++b) {
        const int d = (first + b) % 4;
        const int start_y = y + h - 2 - (b % 2);
        int px = x, py = start_y, pz = z;
        const int len = len_dist(rng);
        for (int k = 1; k <= len; ++k) {
            px += kDx[d];
            pz += kDz[d];
            if (k % 2 == 0) ++py;               // ветка лезет вверх медленно: под ней проход
            logs.push_back({px, py, pz, Block_Types::Oak_Log});
        }
        leaf_blob(leaves, px, py + 1, pz, rad_dist(rng), rng, Block_Types::Oak_Leaf);
    }

    // Главная крона: большой шар над стволом и один-два шара чуть в сторону.
    leaf_blob(leaves, x, y + h + 1, z, 3, rng, Block_Types::Oak_Leaf);
    leaf_blob(leaves, x + off_dist(rng), y + h, z + off_dist(rng), 2, rng, Block_Types::Oak_Leaf);

    for (const Placed& p : leaves) setter(p.x, p.y, p.z, p.type);
    for (const Placed& p : logs) setter(p.x, p.y, p.z, p.type);
}

// Birch is deliberately tall, thin and layered instead of being an oak recolor.
void Tree_Generator::place_birch_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hdist(7, 11);
    const int h = hdist(rng);

    for (int i = 1; i <= h; ++i) setter(x, y + i, z, Block_Types::Birch_Log);

    for (int level = h - 1; level >= 4; level -= 2) {
        const int radius = (level >= h - 2) ? 1 : 2;
        disk(setter, x, y + level, z, radius, Block_Types::Birch_Leaves, true);
    }
    setter(x, y + h + 1, z, Block_Types::Birch_Leaves);
    setter(x + 1, y + h, z, Block_Types::Birch_Leaves);
    setter(x - 1, y + h - 1, z, Block_Types::Birch_Leaves);
}

// Spruce: a classic cone. Tiers are widest at the bottom and narrow to a point
// at the top, so the crown never overhangs the trunk above the lowest tiers.
void Tree_Generator::place_spruce_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hdist(8, 12);
    const int h = hdist(rng);

    for (int i = 1; i <= h; ++i) setter(x, y + i, z, Block_Types::Spruce_Log);

    for (int level = 2; level <= h - 1; level += 2) {
        // Radius 3 at the bottom tier, shrinking by one every ~3 blocks up to 1.
        const int radius = std::clamp(1 + (h - level) / 3, 1, 3);
        disk(setter, x, y + level, z, radius, Block_Types::Spruce_Leaves, false);
        if (level > 3) {
            disk(setter, x, y + level - 1, z, std::max(1, radius - 1), Block_Types::Spruce_Leaves, true);
        }
    }
    setter(x, y + h, z, Block_Types::Spruce_Leaves);
    setter(x, y + h + 1, z, Block_Types::Spruce_Leaves);
}

// Большая хвойная крона строится непрерывным сужающимся силуэтом с редкими
// выраженными юбками. Небольшой шум края сохраняет естественный вид.
void Tree_Generator::place_spruce_giant(int cx, int base_y, int cz, int trunk_size, int height,
                                        int max_radius, Block_Setter setter, Fast_Rng& rng) {
    const int lo = -(trunk_size - 1) / 2;
    const int hi = lo + trunk_size - 1;
    const int core = trunk_size + 2;

    for (int i = 1; i <= height; ++i) {
        for (int dz = lo; dz <= hi; ++dz) {
            for (int dx = lo; dx <= hi; ++dx) {
                setter(cx + dx, base_y + i, cz + dz, Block_Types::Spruce_Log);
            }
        }
    }

    constexpr int crown_bottom = 3;
    const int top = height;
    const int span = std::max(1, top - crown_bottom);
    // Юбки распределены по нижним 60% кроны. Между ними крона остаётся
    // сплошной, а у основания есть небольшой просвет до первой юбки.
    const int crown_height = top - crown_bottom;
    const int first_skirt = crown_bottom + std::max(1, static_cast<int>(std::lround(crown_height * 0.07)));
    const int skirt_spacing = std::max(5, crown_height / 6);

    for (int level = crown_bottom; level <= top; ++level) {
        const double t = static_cast<double>(top - level) / span;
        const double profile = std::pow(t, 0.72);
        const int nearest_skirt = static_cast<int>(std::lround(
            static_cast<double>(level - first_skirt) / skirt_spacing));
        const int skirt_center = first_skirt + nearest_skirt * skirt_spacing;
        const double skirt_distance = std::abs(level - skirt_center);
        const double skirt_bulge = (level >= first_skirt && t > 0.35 && skirt_distance <= 2.0)
            ? 1.8 * (1.0 - skirt_distance / 3.0)
            : 0.0;
        const double ideal_radius = 1.0 + (max_radius - 1) * profile + skirt_bulge;
        const int radius = std::clamp(static_cast<int>(std::lround(ideal_radius)), 1, max_radius);
        const bool upper_crown = t <= 0.35;
        const bool skirt = skirt_bulge > 1.0;
        const int rx = radius;
        const int rz = radius;

        for (int dz = -rz; dz <= rz; ++dz) {
            for (int dx = -rx; dx <= rx; ++dx) {
                const double e = std::sqrt(static_cast<double>(dx * dx) / (rx * rx) +
                                           static_cast<double>(dz * dz) / (rz * rz));
                if (e > 1.0) continue;
                const bool in_trunk = dx >= lo && dx <= hi && dz >= lo && dz <= hi;
                const bool near_trunk = dx * dx + dz * dz <= core * core;
                if (in_trunk) {
                    if (!skirt) setter(cx + dx, base_y + level, cz + dz, Block_Types::Spruce_Leaves);
                    continue;
                }

                // Вырез юбки ограничен нижней частью кроны; открываем только
                // внутреннюю зону, оставляя непрерывное ядро вокруг ствола.
                if (skirt && !near_trunk && e < 0.68) continue;
                setter(cx + dx, base_y + level, cz + dz, Block_Types::Spruce_Leaves);
            }
        }
    }

    // Отдельная округлая верхушка: крайний уровень с радиусом 1 не покрывает
    // углы квадратного ствола 3x3, поэтому без шапки сверху остаются видны брёвна.
    disk(setter, cx, base_y + top, cz, 2, Block_Types::Spruce_Leaves);
    disk(setter, cx, base_y + top + 1, cz, 1, Block_Types::Spruce_Leaves);
}

// Средняя ель: саженцы 2x2. Высота 20-40, ширина до 18 (радиус 8).
void Tree_Generator::place_spruce_medium(int cx, int base_y, int cz, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hdist(20, 40);
    place_spruce_giant(cx, base_y, cz, 2, hdist(rng), 8, setter, rng);
}

// Большая ель: саженцы 3x3. Высота 35-80, ширина до 30 (радиус 14).
void Tree_Generator::place_spruce_large(int cx, int base_y, int cz, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hdist(35, 80);
    place_spruce_giant(cx, base_y, cz, 3, hdist(rng), 14, setter, rng);
}

// Jungle: tall trunk, multiple large horizontal branches and a broad crown.
void Tree_Generator::place_jungle_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hdist(10, 15);
    const int h = hdist(rng);

    for (int i = 1; i <= h; ++i) setter(x, y + i, z, Block_Types::Jungle_Log);

    branch(setter, x, y + h - 3, z,  1,  0, 4, Block_Types::Jungle_Log);
    branch(setter, x, y + h - 5, z, -1,  0, 3, Block_Types::Jungle_Log);
    branch(setter, x, y + h - 4, z,  0,  1, 4, Block_Types::Jungle_Log);
    branch(setter, x, y + h - 6, z,  0, -1, 3, Block_Types::Jungle_Log);

    disk(setter, x, y + h, z, 3, Block_Types::Jungle_Leaves);
    disk(setter, x + 2, y + h - 1, z, 2, Block_Types::Jungle_Leaves);
    disk(setter, x - 2, y + h - 2, z, 2, Block_Types::Jungle_Leaves);
    disk(setter, x, y + h - 1, z + 2, 2, Block_Types::Jungle_Leaves);
    setter(x, y + h + 1, z, Block_Types::Jungle_Leaves);
}

// Acacia: intentionally bent trunk and a flat umbrella crown.
void Tree_Generator::place_acacia_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hdist(5, 8);
    std::uniform_int_distribution<int> dir_dist(0, 3);
    const int h = hdist(rng);
    const int dir = dir_dist(rng);
    const int dx = (dir == 0) - (dir == 1);
    const int dz = (dir == 2) - (dir == 3);

    int cx = x;
    int cz = z;
    for (int i = 1; i <= h; ++i) {
        if (i == h / 2 || i == h - 1) { cx += dx; cz += dz; }
        setter(cx, y + i, cz, Block_Types::Acacia_Log);
    }

    branch(setter, cx, y + h, cz,  1,  0, 3, Block_Types::Acacia_Log);
    branch(setter, cx, y + h, cz, -1,  0, 3, Block_Types::Acacia_Log);
    branch(setter, cx, y + h, cz,  0,  1, 3, Block_Types::Acacia_Log);
    branch(setter, cx, y + h, cz,  0, -1, 3, Block_Types::Acacia_Log);

    disk(setter, cx, y + h + 1, cz, 3, Block_Types::Acacia_Leaves);
    disk(setter, cx, y + h + 2, cz, 2, Block_Types::Acacia_Leaves, true);
}

// Palm: curved trunk and radial fronds, clearly unlike the other trees.
void Tree_Generator::place_palm_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hdist(8, 12);
    std::uniform_int_distribution<int> bend_dist(-1, 1);
    const int h = hdist(rng);
    const int bend = bend_dist(rng);

    int cx = x;
    for (int i = 1; i <= h; ++i) {
        if (i == h / 3 || i == (2 * h) / 3) cx += bend;
        setter(cx, y + i, z, Block_Types::Oak_Log);
    }

    const int top_y = y + h + 1;
    const int frond_count = 8;
    for (int i = 0; i < frond_count; ++i) {
        const double a = (6.28318530718 * i) / frond_count;
        const int fx = static_cast<int>(std::round(std::cos(a) * 4.0));
        const int fz = static_cast<int>(std::round(std::sin(a) * 4.0));
        for (int j = 1; j <= 4; ++j) {
            const int px = cx + static_cast<int>(std::round(std::cos(a) * j));
            const int pz = z  + static_cast<int>(std::round(std::sin(a) * j));
            setter(px, top_y - (j > 2 ? 1 : 0), pz, Block_Types::Oak_Leaf);
        }
        setter(cx + fx, top_y - 1, z + fz, Block_Types::Oak_Leaf);
    }
    setter(cx, top_y, z, Block_Types::Oak_Leaf);
}

// Slimewood: chunky, twisted trunk and a glowing-looking asymmetric crown.
// Слизовое дерево — низкая плакучая ива: короткий ствол, четыре ветки (по одной на сторону света)
// на разных высотах. Каждая ветка сначала поднимается на блок, затем идёт вбок. На её конце —
// широкая плоская шапка листвы, с которой свисают две листовые нити. Сверху — широкая крона
// на верхушке ствола. Листва пишется раньше брёвен, поэтому ствол и ветки не перекрываются.
void Tree_Generator::place_slimewood_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hdist(3, 4);
    std::uniform_int_distribution<int> len_dist(3, 4);
    std::uniform_int_distribution<int> rad_dist(2, 3);
    std::uniform_int_distribution<int> hang_dist(2, 4);
    std::uniform_int_distribution<int> pct(0, 99);
    const int h = hdist(rng);

    // Стороны света: север, восток, юг, запад.
    constexpr int kDx[4] = {0, 1, 0, -1};
    constexpr int kDz[4] = {-1, 0, 1, 0};

    // Высоты отхода веток — все разные (от h-2 до h+1), порядок сторон случайный.
    int heights[4] = {h - 2, h - 1, h, h + 1};
    std::shuffle(heights, heights + 4, rng);

    std::vector<Placed> logs;
    std::vector<Placed> leaves;
    for (int i = 1; i <= h; ++i) logs.push_back({x, y + i, z, Block_Types::Slimewood_Log});

    for (int d = 0; d < 4; ++d) {
        const int attach_y = y + heights[d];
        int px = x, pz = z, py = attach_y;
        const int len = len_dist(rng);
        for (int k = 1; k <= len; ++k) {
            px += kDx[d];
            pz += kDz[d];
            if (k == 1) ++py;                    // первый шаг поднимает ветку, дальше она идёт ровно
            logs.push_back({px, py, pz, Block_Types::Slimewood_Log});
        }

        // Шапка на конце ветки и нити, которые свисают с её краёв.
        const int r = rad_dist(rng);
        const int cap_y = py;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                for (int dz = -r; dz <= r; ++dz) {
                    const int d2 = dx * dx + dz * dz;
                    if (d2 > r * r) continue;
                    const bool edge = d2 > (r - 1) * (r - 1);
                    if (edge && pct(rng) < 40) continue;   // рваный край
                    if (dy == 1 && edge) continue;         // шапка плоская сверху
                    leaves.push_back({px + dx, cap_y + dy, pz + dz, Block_Types::Slimewood_Leaves});
                }
            }
        }
        const int perp_x = kDz[d] != 0 ? 1 : 0;
        const int perp_z = kDx[d] != 0 ? 1 : 0;
        for (int side : {-1, 1}) {
            const int sx = px + kDx[d] * 1 + perp_x * side * 2;
            const int sz = pz + kDz[d] * 1 + perp_z * side * 2;
            const int hang = hang_dist(rng);
            for (int j = 0; j <= hang; ++j) {
                leaves.push_back({sx, cap_y - 1 - j, sz, Block_Types::Slimewood_Leaves});
            }
        }
    }

    // Широкая крона на верхушке ствола.
    const int top_r = 3;
    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = -top_r; dx <= top_r; ++dx) {
            for (int dz = -top_r; dz <= top_r; ++dz) {
                const int d2 = dx * dx + dz * dz;
                if (d2 > top_r * top_r) continue;
                if (d2 > (top_r - 1) * (top_r - 1) && pct(rng) < 35) continue;
                leaves.push_back({x + dx, y + h + dy, z + dz, Block_Types::Slimewood_Leaves});
            }
        }
    }

    for (const Placed& p : leaves) setter(p.x, p.y, p.z, p.type);
    for (const Placed& p : logs) setter(p.x, p.y, p.z, p.type);
}
