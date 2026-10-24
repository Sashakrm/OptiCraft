#include "world/blocks/Block_Shapes.h"

uint8_t stairs_high_side(uint8_t meta) {
    return static_cast<uint8_t>(meta & 3u);
}

uint8_t opposite_facing(uint8_t meta) {
    return static_cast<uint8_t>((meta + 2u) & 3u);
}

int shape_boxes(Block_Shape shape, uint8_t meta, Shape_Box out[2]) {
    switch (shape) {
        case Block_Shape::Slab:
            out[0] = {glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.5f, 1.0f)};
            return 1;
        case Block_Shape::Stairs: {
            out[0] = {glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.5f, 1.0f)};
            // Верхняя половина: четверть клетки со стороны высокой стороны.
            switch (stairs_high_side(meta)) {
                case Block_Meta::facing_north: out[1] = {glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(1.0f, 1.0f, 0.5f)}; break;
                case Block_Meta::facing_east:  out[1] = {glm::vec3(0.5f, 0.5f, 0.0f), glm::vec3(1.0f, 1.0f, 1.0f)}; break;
                case Block_Meta::facing_south: out[1] = {glm::vec3(0.0f, 0.5f, 0.5f), glm::vec3(1.0f, 1.0f, 1.0f)}; break;
                default:                       out[1] = {glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f, 1.0f, 1.0f)}; break;
            }
            return 2;
        }
        case Block_Shape::Plate:
            // Пластинка 1/16 толщиной у пола (медный провод).
            out[0] = {glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 1.0f / 16.0f, 1.0f)};
            return 1;
        case Block_Shape::None:
        default:
            return 0;
    }
}
