#include "world/generation/Structures.h"
#include "world/generation/Tree_Generator.h"

void Structure_Generator::create_from_id(Structure_Type type, int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    switch (type) {
        case Structure_Type::Oak_Tree:
            Tree_Generator::place_oak_tree(x, y, z, setter, rng);
            break;
        case Structure_Type::Acacia_Tree:
            Tree_Generator::place_acacia_tree(x, y, z, setter, rng);
            break;
        case Structure_Type::Palm_Tree:
            Tree_Generator::place_palm_tree(x, y, z, setter, rng);
            break;
        case Structure_Type::Pyramid:
            make_pyramid(x, y, z, setter, rng);
            break;
    }
}

void Structure_Generator::make_pyramid(int x, int y, int z, Block_Setter setter, Fast_Rng& rng) {
    (void)rng;

    for (int base = 9, h = 0; base > 0; base -= 2, ++h) {
        for (int dx = -base / 2; dx < base / 2; ++dx) {
            for (int dz = -base / 2; dz < base / 2; ++dz) {
                setter(x + dx, y + h, z + dz, Block_Types::Stone);
            }
        }
    }
}
