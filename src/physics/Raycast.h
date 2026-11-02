#ifndef OPTICRAFT_RAYCAST_H
#define OPTICRAFT_RAYCAST_H

#include <glm/glm.hpp>
#include "world/blocks/Block_Types.h"

class Chunk_Manager;

struct Raycast_Hit {
    bool hit = false;
    int block_x = 0;
    int block_y = 0;
    int block_z = 0;
    int place_x = 0;
    int place_y = 0;
    int place_z = 0;
    float distance = 0.0f;
    Block_Types block_type = Block_Types::Air;
    // Для блоков с точным хитбоксом (кнопка, провод): бокс, в который попал луч, в мировых координатах.
    bool has_box = false;
    glm::vec3 box_lo{0.0f};
    glm::vec3 box_hi{1.0f};
};

class Raycast {
public:
    static Raycast_Hit cast(const glm::vec3& origin,
                            const glm::vec3& direction,
                            float max_distance,
                            const Chunk_Manager& chunk_manager);
};

#endif
