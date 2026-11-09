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
        case Tree_Type::Beech:     place_beech_tree(x, y, z, setter, rng); break;
        case Tree_Type::Redwood:   place_redwood_tree(x, y, z, setter, rng); break;
        case Tree_Type::Mangrove:  place_mangrove_tree(x, y, z, setter, rng); break;
        case Tree_Type::Willow:   place_willow_tree(x, y, z, setter, rng); break;
        case Tree_Type::Skywood:  place_skywood_tree(x, y, z, setter, rng); break;
        case Tree_Type::Glowwood: place_glowwood_cave_tree(x, z, y, y + 24, setter, rng); break;
        case Tree_Type::Baobab:   place_baobab_tree(x, y, z, setter, rng); break;
        case Tree_Type::Cherry:   place_cherry_tree(x, y, z, setter, rng); break;
        case Tree_Type::Aspen:    place_aspen_tree(x, y, z, setter, rng); break;
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
        setter(cx, y + i, z, Block_Types::Palm_Log);
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
            setter(px, top_y - (j > 2 ? 1 : 0), pz, Block_Types::Palm_Leaves);
        }
        setter(cx + fx, top_y - 1, z + fz, Block_Types::Palm_Leaves);
    }
    setter(cx, top_y, z, Block_Types::Palm_Leaves);
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


// Буковое дерево: широкая плотная куполообразная крона и несколько коротких боковых ветвей.
void Tree_Generator::place_beech_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hd(8, 12), rd(3, 4), shift(-1, 1);
    const int h = hd(rng);
    const int canopy_shift = shift(rng);
    for (int i = 1; i <= h; ++i) setter(x, y + i, z, Block_Types::Beech_Log);
    for (int d = 0; d < 4; ++d) {
        const int dx = (d == 0) - (d == 1);
        const int dz = (d == 2) - (d == 3);
        const int len = 2 + (d & 1);
        for (int i = 1; i <= len; ++i)
            setter(x + dx * i, y + h - 3 + i / 3, z + dz * i, Block_Types::Beech_Log);
    }
    const int radius = rd(rng);
    for (int dy = -2; dy <= 2; ++dy) {
        const int r = std::max(1, radius - (std::abs(dy) == 2 ? 1 : 0));
        for (int dx = -r; dx <= r; ++dx) for (int dz = -r; dz <= r; ++dz) {
            if (dx * dx + dz * dz > r * r + 1) continue;
            if (dy == -2 && (dx + dz) % 3 == 0) continue;
            setter(x + canopy_shift + dx, y + h + dy, z + dz, Block_Types::Beech_Leaves);
        }
    }
}


// Редвуд: гигантский прямой ствол, четыре широких яруса ветвей и верхняя плоская крона.
void Tree_Generator::place_redwood_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hd(24, 32), radius_dist(4, 6);
    const int h = hd(rng);
    const int base = (h > 28) ? 1 : 0;
    for (int dy = 1; dy <= h; ++dy) {
        for (int dx = -base; dx <= base; ++dx)
            for (int dz = -base; dz <= base; ++dz)
                setter(x + dx, y + dy, z + dz, Block_Types::Redwood_Log);
    }
    for (int tier = 0; tier < 4; ++tier) {
        const int level = h - 7 + tier * 2;
        const int radius = radius_dist(rng) - (tier == 3 ? 1 : 0);
        disk(setter, x, y + level, z, radius, Block_Types::Redwood_Leaves);
        for (int d = 0; d < 4; ++d) {
            const int dx = (d == 0) - (d == 1);
            const int dz = (d == 2) - (d == 3);
            const int len = radius - 1;
            for (int i = 1; i <= len; ++i)
                setter(x + dx * i, y + level + i / 4, z + dz * i, Block_Types::Redwood_Log);
            disk(setter, x + dx * len, y + level + 1, z + dz * len,
                 std::max(2, radius - 1), Block_Types::Redwood_Leaves);
        }
    }
    disk(setter, x, y + h, z, 3, Block_Types::Redwood_Leaves);
}

// Мангровое дерево: ствол над водой, расходящиеся опорные корни и компактная зелёная крона.
void Tree_Generator::place_mangrove_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hd(7, 10), root_len(2, 4), crown(3, 4);
    const int h = hd(rng);
    for (int i = 1; i <= h; ++i) setter(x, y + i, z, Block_Types::Mangrove_Log);
    constexpr int dxs[8] = {1,-1,0,0,1,1,-1,-1};
    constexpr int dzs[8] = {0,0,1,-1,1,-1,1,-1};
    for (int d = 0; d < 8; ++d) {
        const int len = root_len(rng);
        for (int i = 1; i <= len; ++i) {
            const int py = y + std::max(0, 3 - i / 2);
            setter(x + dxs[d] * i, py, z + dzs[d] * i, Block_Types::Mangrove_Root);
        }
    }
    disk(setter, x, y + h, z, crown(rng), Block_Types::Mangrove_Leaves);
    for (int d = 0; d < 4; ++d) {
        const int dx = (d == 0) - (d == 1);
        const int dz = (d == 2) - (d == 3);
        setter(x + dx * 2, y + h - 1, z + dz * 2, Block_Types::Mangrove_Log);
        for (int j = 0; j < 3; ++j)
            setter(x + dx * 3, y + h - 2 - j, z + dz * 3, Block_Types::Mangrove_Leaves);
    }
}

// Ива: ветви поднимаются наружу, после чего тонкие побеги свисают ниже кроны.
void Tree_Generator::place_willow_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hd(7, 10), len_dist(3, 5), hang_dist(4, 7);
    const int h = hd(rng);
    for (int i = 1; i <= h; ++i) setter(x, y + i, z, Block_Types::Willow_Log);
    constexpr int dxs[4] = {1,0,-1,0}, dzs[4] = {0,1,0,-1};
    for (int d = 0; d < 4; ++d) {
        const int len = len_dist(rng);
        int ex = x, ez = z, ey = y + h - 2;
        for (int i = 1; i <= len; ++i) {
            ex += dxs[d]; ez += dzs[d];
            if (i % 2 == 0) ++ey;
            setter(ex, ey, ez, Block_Types::Willow_Log);
        }
        const int hang = hang_dist(rng);
        for (int j = 0; j <= hang; ++j) {
            setter(ex, ey - j, ez, Block_Types::Willow_Leaves);
            if ((j & 1) == 0) {
                setter(ex + dxs[d], ey - j, ez + dzs[d], Block_Types::Willow_Leaves);
                setter(ex - dzs[d], ey - j, ez + dxs[d], Block_Types::Willow_Leaves);
            }
        }
    }
    disk(setter, x, y + h + 1, z, 2, Block_Types::Willow_Leaves);
}

// Небесное дерево: изогнутый ствол, облачная крона и свисающие побеги.
void Tree_Generator::place_skywood_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hd(8, 13), bend(-1, 1), rd(3, 5);
    const int h = hd(rng);
    int cx = x, cz = z;
    for (int i = 1; i <= h; ++i) {
        if (i % 4 == 0) { cx += bend(rng); cz += bend(rng); }
        setter(cx, y + i, cz, Block_Types::Skywood_Log);
    }
    const int r = rd(rng);
    for (int dy = -2; dy <= 2; ++dy) {
        const int layer = std::max(1, r - (std::abs(dy) == 2 ? 2 : 0));
        for (int dx = -layer; dx <= layer; ++dx)
            for (int dz = -layer; dz <= layer; ++dz)
                if (dx * dx + dz * dz <= layer * layer + 1)
                    setter(cx + dx, y + h + dy, cz + dz, Block_Types::Skywood_Leaves);
    }
    for (int d = 0; d < 6; ++d) {
        const double a = d * 1.0471975512;
        const int dx = static_cast<int>(std::round(std::cos(a) * (r - 1)));
        const int dz = static_cast<int>(std::round(std::sin(a) * (r - 1)));
        for (int j = 1; j <= 4; ++j)
            setter(cx + dx, y + h - 1 - j, cz + dz, Block_Types::Sky_Vine);
    }
}

// Светящееся пещерное дерево: ствол тянется почти от пола до потолка, а ветви
// образуют несколько ярусов биолюминесцентной кроны на всей высоте зала.
void Tree_Generator::place_glowwood_cave_tree(int x, int z, int floor_y, int ceiling_y,
                                               Block_Setter setter, Fast_Rng& rng) {
    const int bottom = floor_y + 1;
    const int top = ceiling_y - 1;
    const int h = top - bottom + 1;
    if (h < 8) return;
    for (int y = bottom; y <= top; ++y) {
        setter(x, y, z, Block_Types::Glowwood_Log);
    }
    // Короткие ветви распределены по всей длине ствола, чтобы растение занимало зал
    // от пола до потолка, а не превращалось в обычную крону у верхушки.
    const int stride = std::max(3, h / 5);
    for (int level = bottom + 2, tier = 0; level < top - 1; level += stride, ++tier) {
        const int reach = (tier % 2 == 0) ? 3 : 2;
        for (int d = 0; d < 4; ++d) {
            const int dx = (d == 0) - (d == 1);
            const int dz = (d == 2) - (d == 3);
            int bx = x, bz = z;
            for (int i = 1; i <= reach; ++i) {
                bx = x + dx * i; bz = z + dz * i;
                setter(bx, level + (i > 2 ? 1 : 0), bz, Block_Types::Glowwood_Log);
            }
            for (int ox = -1; ox <= 1; ++ox) for (int oz = -1; oz <= 1; ++oz)
                setter(bx + ox, level + 1, bz + oz, Block_Types::Glowwood_Leaves);
            // Нити со светящимися плодами создают отдельные точки света по высоте комнаты.
            const int hang = 1 + static_cast<int>(rng() % 4u);
            for (int j = 1; j <= hang; ++j)
                setter(bx, level - j, bz, j == hang ? Block_Types::Glow_Berry : Block_Types::Glowwood_Leaves);
        }
        if ((tier & 1) == 0) {
            for (int dx = -1; dx <= 1; ++dx) for (int dz = -1; dz <= 1; ++dz)
                setter(x + dx, level + 2, z + dz, Block_Types::Glowwood_Leaves);
            setter(x, level + 2, z, Block_Types::Glow_Bloom);
        }
    }
    // Верхушка также светится, но оставляет зазор до потолка и не ломает породу.
    disk(setter, x, top - 1, z, 2, Block_Types::Glowwood_Leaves);
    setter(x, top, z, Block_Types::Glow_Bloom);
}

// Баобаб: массивный бочкообразный ствол и небольшая разреженная крона высоко над ним.
void Tree_Generator::place_baobab_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hd(7, 10), rdist(2, 3);
    const int h = hd(rng);
    for (int dy = 1; dy <= h; ++dy) {
        const int r = (dy < 3) ? 2 : 1;
        for (int dx = -r; dx <= r; ++dx) for (int dz = -r; dz <= r; ++dz)
            if (dx * dx + dz * dz <= r * r + 1)
                setter(x + dx, y + dy, z + dz, Block_Types::Baobab_Log);
    }
    const int r = rdist(rng);
    disk(setter, x, y + h + 1, z, r, Block_Types::Baobab_Leaves);
    for (int d = 0; d < 4; ++d) {
        const int dx = (d == 0) - (d == 1);
        const int dz = (d == 2) - (d == 3);
        for (int i = 1; i <= 3; ++i)
            setter(x + dx * i, y + h - 1 + i / 2, z + dz * i, Block_Types::Baobab_Log);
    }
}


// Сакура: разветвлённый ствол и несколько розовых облаков цветения, разделённых воздухом.
void Tree_Generator::place_cherry_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hd(7, 10), len(2, 4), radius(2, 3), bend(-1, 1);
    const int h = hd(rng);
    int tx = x, tz = z;
    for (int i = 1; i <= h; ++i) {
        if (i == h / 2 || i == (h * 3) / 4) { tx += bend(rng); tz += bend(rng); }
        setter(tx, y + i, tz, Block_Types::Cherry_Log);
    }
    constexpr int dxs[4] = {1,0,-1,0}, dzs[4] = {0,1,0,-1};
    for (int d = 0; d < 4; ++d) {
        int ex = tx, ez = tz, ey = y + h - 2;
        const int n = len(rng);
        for (int i = 1; i <= n; ++i) {
            ex += dxs[d]; ez += dzs[d];
            if ((i & 1) == 0) ++ey;
            setter(ex, ey, ez, Block_Types::Cherry_Log);
        }
        const int r = radius(rng);
        for (int dy = -1; dy <= 1; ++dy)
            for (int ox = -r; ox <= r; ++ox)
                for (int oz = -r; oz <= r; ++oz)
                    if (ox * ox + oz * oz <= r * r + 1)
                        setter(ex + ox, ey + dy, ez + oz, Block_Types::Cherry_Leaves);
    }
    disk(setter, tx, y + h + 1, tz, 3, Block_Types::Cherry_Leaves);
}


// Осина: высокая и сравнительно узкая крона, прямой светлый ствол и несколько
// тонких ветвей. По силуэту заметно отличается от широкой берёзы и бука.
void Tree_Generator::place_aspen_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    std::uniform_int_distribution<int> hd(12, 17), branch_len(2, 3), crown_radius(2, 3);
    const int h = hd(rng);
    int cx = x, cz = z;
    for (int dy = 1; dy <= h; ++dy) {
        if (dy == h / 2 || dy == (h * 3) / 4) {
            if ((rng() & 3u) == 0u) cx += (rng() & 1u) ? 1 : -1;
            if ((rng() & 3u) == 0u) cz += (rng() & 1u) ? 1 : -1;
        }
        setter(cx, y + dy, cz, Block_Types::Aspen_Log);
    }

    constexpr int dxs[4] = {1, 0, -1, 0};
    constexpr int dzs[4] = {0, 1, 0, -1};
    for (int d = 0; d < 4; ++d) {
        const int level = y + h - 3 - (d & 1) * 2;
        const int len = branch_len(rng);
        for (int i = 1; i <= len; ++i)
            setter(cx + dxs[d] * i, level + i / 3, cz + dzs[d] * i, Block_Types::Aspen_Log);
        const int r = crown_radius(rng);
        disk(setter, cx + dxs[d] * len, level + 1, cz + dzs[d] * len, r, Block_Types::Aspen_Leaves);
    }
    // Верхняя крона вытянута по вертикали: три небольших слоя вместо широкого шара.
    for (int dy = -1; dy <= 2; ++dy) {
        const int r = (dy == -1 || dy == 2) ? 1 : 2;
        disk(setter, cx, y + h + dy, cz, r, Block_Types::Aspen_Leaves);
    }
}
