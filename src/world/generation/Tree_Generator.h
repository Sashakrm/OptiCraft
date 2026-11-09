#ifndef OPTICRAFT_TREE_GENERATOR_H
#define OPTICRAFT_TREE_GENERATOR_H

#include <functional>
#include <random>
#include "math/Fast_Rng.h"
#include "world/blocks/Block_Types.h"

enum class Tree_Type : int {
    Oak = 0,
    Palm = 1,
    Acacia = 2,
    Birch = 3,
    Spruce = 4,
    Jungle = 5,
    Slimewood = 6,
    Beech = 7,
    Redwood = 8,
    Mangrove = 9,
    Willow = 10,
    Skywood = 11,
    Glowwood = 12,
    Baobab = 13,
    Cherry = 14,
    Aspen = 15
};

class Tree_Generator {
public:
    using Block_Setter = std::function<void(int x, int y, int z, Block_Types type)>;

    static void place_tree(Tree_Type type, int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_oak_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_palm_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_acacia_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_birch_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_spruce_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    // Гигантские ели из саженцев (см. Sapling_Manager). cx/cz — центр ствола, base_y — блок земли
    // под ним. Высота и ширина выбираются процедурно. Ствол квадратный: trunk_size 2 или 3.
    static void place_spruce_medium(int cx, int base_y, int cz, Block_Setter setter, Fast_Rng& rng);
    static void place_spruce_large(int cx, int base_y, int cz, Block_Setter setter, Fast_Rng& rng);
    static void place_spruce_giant(int cx, int base_y, int cz, int trunk_size, int height, int max_radius,
                                   Block_Setter setter, Fast_Rng& rng);
    static void place_jungle_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_slimewood_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_beech_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_redwood_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_mangrove_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_willow_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_skywood_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_glowwood_cave_tree(int x, int z, int floor_y, int ceiling_y,
                                         Block_Setter setter, Fast_Rng& rng);
    static void place_baobab_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_cherry_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void place_aspen_tree(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
};

#endif
