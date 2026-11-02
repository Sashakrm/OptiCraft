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
        case Block_Shape::Wire:
            // Провод: 12 на 12 пикселей текстуры, высота 2 пикселя, по центру клетки у пола.
            out[0] = {glm::vec3(2.0f / 16.0f, 0.0f, 2.0f / 16.0f), glm::vec3(14.0f / 16.0f, 2.0f / 16.0f, 14.0f / 16.0f)};
            return 1;
        case Block_Shape::Button: {
            // Кнопка 3 (ширина) x 2 (высота) x 2 (вынос от опоры) пикселя по центру грани крепления.
            // meta & 7 — индекс стороны опоры: +X, -X, +Y, -Y, +Z, -Z (как kDirs в Logic_Manager).
            constexpr float p = 1.0f / 16.0f;
            constexpr float w0 = 6.5f * p, w1 = 9.5f * p;   // ширина 3 пикселя
            constexpr float h0 = 7.0f * p, h1 = 9.0f * p;   // высота / глубина 2 пикселя
            switch (meta & 7u) {
                case 0:  out[0] = {glm::vec3(1.0f - 2.0f * p, h0, w0), glm::vec3(1.0f, h1, w1)}; break;          // опора +X
                case 1:  out[0] = {glm::vec3(0.0f, h0, w0), glm::vec3(2.0f * p, h1, w1)}; break;                 // опора -X
                case 2:  out[0] = {glm::vec3(w0, 1.0f - 2.0f * p, h0), glm::vec3(w1, 1.0f, h1)}; break;          // опора +Y
                case 4:  out[0] = {glm::vec3(w0, h0, 1.0f - 2.0f * p), glm::vec3(w1, h1, 1.0f)}; break;          // опора +Z
                case 5:  out[0] = {glm::vec3(w0, h0, 0.0f), glm::vec3(w1, h1, 2.0f * p)}; break;                 // опора -Z
                default: out[0] = {glm::vec3(w0, 0.0f, h0), glm::vec3(w1, 2.0f * p, h1)}; break;                 // опора -Y (пол)
            }
            return 1;
        }
        case Block_Shape::None:
        default:
            return 0;
    }
}
