#include "world/blocks/Block_Shapes.h"

uint8_t stairs_high_side(uint8_t meta) {
    return static_cast<uint8_t>(meta & 3u);
}

uint8_t opposite_facing(uint8_t meta) {
    return static_cast<uint8_t>((meta + 2u) & 3u);
}

uint8_t stairs_effective_meta(uint8_t meta, bool front_is_stairs, uint8_t front_meta,
                              bool back_is_stairs, uint8_t back_meta) {
    const uint8_t facing = static_cast<uint8_t>(meta & 3u);
    const uint8_t left = static_cast<uint8_t>((facing + 3u) & 3u);
    const uint8_t right = static_cast<uint8_t>((facing + 1u) & 3u);

    // A perpendicular stair in front makes an outer corner. A perpendicular stair behind
    // makes an inner corner. Front takes precedence in a compact 2x2 stair junction.
    uint8_t corner = 0u;
    uint8_t side = 0u; // 0 = left, 1 = right
    if (front_is_stairs) {
        const uint8_t neighbor_facing = static_cast<uint8_t>(front_meta & 3u);
        if (neighbor_facing == left) { corner = 1u; side = 0u; }
        else if (neighbor_facing == right) { corner = 1u; side = 1u; }
    }
    if (corner == 0u && back_is_stairs) {
        const uint8_t neighbor_facing = static_cast<uint8_t>(back_meta & 3u);
        if (neighbor_facing == left) { corner = 2u; side = 0u; }
        else if (neighbor_facing == right) { corner = 2u; side = 1u; }
    }
    return static_cast<uint8_t>(facing | (corner << 2u) | (side << 4u));
}

int shape_boxes(Block_Shape shape, uint8_t meta, Shape_Box out[8]) {
    switch (shape) {
        case Block_Shape::Slab:
            out[0] = {glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.5f, 1.0f)};
            return 1;
        case Block_Shape::Stairs: {
            // Four lower quarter-columns cover the bottom half. The upper footprint is a
            // two-quarter strip (straight), one quarter (outer corner), or three quarters
            // (inner corner). Keeping quarters separate lets the mesh shader cull every
            // shared internal face generically; it avoids coplanar overlap/z-fighting.
            constexpr float half = 0.5f;
            for (int qz = 0; qz < 2; ++qz) {
                for (int qx = 0; qx < 2; ++qx) {
                    out[qz * 2 + qx] = {
                        glm::vec3(qx * half, 0.0f, qz * half),
                        glm::vec3((qx + 1) * half, half, (qz + 1) * half)
                    };
                }
            }

            const uint8_t facing = static_cast<uint8_t>(meta & 3u);
            const uint8_t corner = static_cast<uint8_t>((meta >> 2u) & 3u);
            const bool right_side = (meta & 16u) != 0u;
            uint8_t facing_mask = 0u;
            uint8_t side_mask = 0u;
            // Quadrant bits: 0=NW, 1=NE, 2=SW, 3=SE.
            switch (facing) {
                case Block_Meta::facing_north: facing_mask = 0x3u; break;
                case Block_Meta::facing_east:  facing_mask = 0xAu; break;
                case Block_Meta::facing_south: facing_mask = 0xCu; break;
                default:                       facing_mask = 0x5u; break;
            }
            const uint8_t side_direction = right_side
                ? static_cast<uint8_t>((facing + 1u) & 3u)
                : static_cast<uint8_t>((facing + 3u) & 3u);
            switch (side_direction) {
                case Block_Meta::facing_north: side_mask = 0x3u; break;
                case Block_Meta::facing_east:  side_mask = 0xAu; break;
                case Block_Meta::facing_south: side_mask = 0xCu; break;
                default:                       side_mask = 0x5u; break;
            }

            uint8_t upper_mask = facing_mask;
            if (corner == 1u) upper_mask = static_cast<uint8_t>(facing_mask & side_mask); // outer L-turn tip
            else if (corner == 2u) upper_mask = static_cast<uint8_t>(facing_mask | side_mask); // inner L-fill
            // Compact upper boxes so the mesh shader never scans holes in the output array.
            int count = 4;
            for (int quadrant = 0; quadrant < 4; ++quadrant) {
                if ((upper_mask & (1u << quadrant)) == 0u) continue;
                const int qx = quadrant & 1;
                const int qz = quadrant >> 1;
                out[count++] = {glm::vec3(qx * half, half, qz * half),
                                glm::vec3((qx + 1) * half, 1.0f, (qz + 1) * half)};
            }
            return count;
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
