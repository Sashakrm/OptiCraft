#include "world/generation/Tree_Generator.h"

#include <algorithm>
#include <cmath>

namespace {
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
                                Block_Setter setter, std::mt19937& rng) {
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

// Wide, asymmetric crown with a short branching trunk.
void Tree_Generator::place_oak_tree(int x, int y, int z, Block_Setter setter, std::mt19937& rng) {
    std::uniform_int_distribution<int> hdist(5, 8);
    const int h = hdist(rng);

    for (int i = 1; i <= h; ++i) setter(x, y + i, z, Block_Types::Oak_Wood);

    branch(setter, x, y + h - 2, z,  1,  0, 2, Block_Types::Oak_Wood);
    branch(setter, x, y + h - 3, z, -1,  1, 2, Block_Types::Oak_Wood);
    branch(setter, x, y + h - 2, z,  0, -1, 2, Block_Types::Oak_Wood);

    disk(setter, x, y + h, z, 2, Block_Types::Oak_Leaf);
    disk(setter, x + 1, y + h - 1, z, 2, Block_Types::Oak_Leaf);
    disk(setter, x - 1, y + h - 1, z + 1, 2, Block_Types::Oak_Leaf);
    setter(x, y + h + 1, z, Block_Types::Oak_Leaf);
}

// Birch is deliberately tall, thin and layered instead of being an oak recolor.
void Tree_Generator::place_birch_tree(int x, int y, int z, Block_Setter setter, std::mt19937& rng) {
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

// Spruce: one strong central cone with dense tiers.
void Tree_Generator::place_spruce_tree(int x, int y, int z, Block_Setter setter, std::mt19937& rng) {
    std::uniform_int_distribution<int> hdist(8, 12);
    const int h = hdist(rng);

    for (int i = 1; i <= h; ++i) setter(x, y + i, z, Block_Types::Spruce_Log);

    for (int level = 2; level <= h; level += 2) {
        const int radius = std::min(4, 1 + level / 3);
        disk(setter, x, y + level, z, radius, Block_Types::Spruce_Leaves, false);
        if (level > 3) {
            disk(setter, x, y + level - 1, z, std::max(1, radius - 1), Block_Types::Spruce_Leaves, true);
        }
    }
    setter(x, y + h + 1, z, Block_Types::Spruce_Leaves);
}

// Jungle: tall trunk, multiple large horizontal branches and a broad crown.
void Tree_Generator::place_jungle_tree(int x, int y, int z, Block_Setter setter, std::mt19937& rng) {
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
void Tree_Generator::place_acacia_tree(int x, int y, int z, Block_Setter setter, std::mt19937& rng) {
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
void Tree_Generator::place_palm_tree(int x, int y, int z, Block_Setter setter, std::mt19937& rng) {
    std::uniform_int_distribution<int> hdist(8, 12);
    std::uniform_int_distribution<int> bend_dist(-1, 1);
    const int h = hdist(rng);
    const int bend = bend_dist(rng);

    int cx = x;
    for (int i = 1; i <= h; ++i) {
        if (i == h / 3 || i == (2 * h) / 3) cx += bend;
        setter(cx, y + i, z, Block_Types::Oak_Wood);
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
void Tree_Generator::place_slimewood_tree(int x, int y, int z, Block_Setter setter, std::mt19937& rng) {
    std::uniform_int_distribution<int> hdist(6, 10);
    const int h = hdist(rng);

    int cx = x;
    int cz = z;
    for (int i = 1; i <= h; ++i) {
        if (i % 3 == 0) {
            cx += (i & 1) ? 1 : -1;
            cz += (i % 4 == 0) ? 1 : 0;
        }
        setter(cx, y + i, cz, Block_Types::Slimewood_Log);
        if (i > 2 && (i % 3 == 0)) setter(cx - 1, y + i, cz, Block_Types::Slimewood_Log);
    }

    disk(setter, cx, y + h, cz, 2, Block_Types::Slimewood_Leaves);
    disk(setter, cx + 2, y + h - 1, cz, 2, Block_Types::Slimewood_Leaves, true);
    disk(setter, cx - 1, y + h - 2, cz + 2, 2, Block_Types::Slimewood_Leaves, true);
    disk(setter, cx, y + h + 1, cz, 1, Block_Types::Slimewood_Leaves);
}
