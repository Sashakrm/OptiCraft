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
    Slimewood = 6
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
};

#endif
