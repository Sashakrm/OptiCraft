#ifndef OPTICRAFT_STRUCTURES_H
#define OPTICRAFT_STRUCTURES_H

#include <functional>
#include "math/Fast_Rng.h"
#include "world/blocks/Block_Types.h"

enum class Structure_Type : int {
    Oak_Tree = 0,
    Acacia_Tree = 1,
    Palm_Tree = 2,
    Pyramid = 100
};

class Structure_Generator {
public:
    using Block_Setter = std::function<void(int x, int y, int z, Block_Types type)>;

    static void create_from_id(Structure_Type type, int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
    static void make_pyramid(int x, int y, int z, Block_Setter setter, Fast_Rng& rng);
};

#endif //OPTICRAFT_STRUCTURES_H
