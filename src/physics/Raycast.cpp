#include "physics/Raycast.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Types.h"
#include "world/blocks/Block_Shapes.h"
#include "utils/Config.h"

#include <cmath>
#include <algorithm>
#include <limits>

Raycast_Hit Raycast::cast(const glm::vec3& origin,
                          const glm::vec3& direction,
                          float max_distance,
                          const Chunk_Manager& chunk_manager) {
    Raycast_Hit result;

    if (glm::length(direction) < 0.0001f) {
        return result;
    }

    glm::vec3 dir = glm::normalize(direction);

    int x = static_cast<int>(std::floor(origin.x));
    int y = static_cast<int>(std::floor(origin.y));
    int z = static_cast<int>(std::floor(origin.z));

    const int step_x = dir.x > 0.0f ? 1 : (dir.x < 0.0f ? -1 : 0);
    const int step_y = dir.y > 0.0f ? 1 : (dir.y < 0.0f ? -1 : 0);
    const int step_z = dir.z > 0.0f ? 1 : (dir.z < 0.0f ? -1 : 0);

    const float t_delta_x = step_x != 0 ? std::abs(1.0f / dir.x) : std::numeric_limits<float>::max();
    const float t_delta_y = step_y != 0 ? std::abs(1.0f / dir.y) : std::numeric_limits<float>::max();
    const float t_delta_z = step_z != 0 ? std::abs(1.0f / dir.z) : std::numeric_limits<float>::max();

    const float next_boundary_x = step_x > 0 ? static_cast<float>(x + 1) : static_cast<float>(x);
    const float next_boundary_y = step_y > 0 ? static_cast<float>(y + 1) : static_cast<float>(y);
    const float next_boundary_z = step_z > 0 ? static_cast<float>(z + 1) : static_cast<float>(z);

    float t_max_x = step_x != 0 ? (next_boundary_x - origin.x) / dir.x : std::numeric_limits<float>::max();
    float t_max_y = step_y != 0 ? (next_boundary_y - origin.y) / dir.y : std::numeric_limits<float>::max();
    float t_max_z = step_z != 0 ? (next_boundary_z - origin.z) / dir.z : std::numeric_limits<float>::max();

    if (t_max_x < 0.0f) t_max_x += t_delta_x;
    if (t_max_y < 0.0f) t_max_y += t_delta_y;
    if (t_max_z < 0.0f) t_max_z += t_delta_z;

    int prev_x = x;
    int prev_y = y;
    int prev_z = z;

    float traveled = 0.0f;

    while (traveled <= max_distance) {
        if (y < Config::world_min_y || y > Config::world_max_y) {
            break;
        }

        Block_Types block = chunk_manager.get_block_world(x, y, z);
        const auto& props = get_block_props(block);

        // Луч останавливается на твёрдых блоках И на флоре (трава, цветы, посевы): она не
        // твёрдая для коллизий, но игрок должен мочь навести на неё и сломать.
        const bool targetable = props.is_solid || props.mesh_style == Block_Mesh_Style::XStyle;
        // Блоки с точным хитбоксом (кнопка, провод): луч попадает только в их боксы, остальная часть клетки
        // прозрачна для прицела.
        bool box_hit = true;
        bool has_box = false;
        float box_t = traveled;
        glm::vec3 hit_lo{0.0f}, hit_hi{1.0f};
        if (block != Block_Types::Air && block != Block_Types::Water && targetable &&
            shape_has_precise_hitbox(props.shape)) {
            box_hit = false;
            Shape_Box boxes[2];
            const int count = shape_boxes(props.shape, chunk_manager.get_block_meta_world(x, y, z), boxes);
            const glm::vec3 cell(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
            float best_t = std::numeric_limits<float>::max();
            for (int b = 0; b < count; ++b) {
                const glm::vec3 lo = cell + boxes[b].lo;
                const glm::vec3 hi = cell + boxes[b].hi;
                float t_near = 0.0f, t_far = max_distance;
                bool miss = false;
                for (int axis = 0; axis < 3 && !miss; ++axis) {
                    if (std::abs(dir[axis]) < 1e-8f) {
                        if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) miss = true;
                    } else {
                        float t0 = (lo[axis] - origin[axis]) / dir[axis];
                        float t1 = (hi[axis] - origin[axis]) / dir[axis];
                        if (t0 > t1) std::swap(t0, t1);
                        t_near = std::max(t_near, t0);
                        t_far = std::min(t_far, t1);
                        if (t_near > t_far) miss = true;
                    }
                }
                if (!miss && t_near < best_t) {
                    best_t = t_near;
                    hit_lo = lo;
                    hit_hi = hi;
                    box_hit = true;
                    has_box = true;
                }
            }
            if (box_hit) box_t = best_t;
        }
        if (block != Block_Types::Air && block != Block_Types::Water && targetable && box_hit) {
            result.hit = true;
            result.block_x = x;
            result.block_y = y;
            result.block_z = z;
            result.place_x = prev_x;
            result.place_y = prev_y;
            result.place_z = prev_z;
            result.distance = box_t;
            result.block_type = block;
            result.has_box = has_box;
            result.box_lo = hit_lo;
            result.box_hi = hit_hi;
            return result;
        }

        prev_x = x;
        prev_y = y;
        prev_z = z;

        if (t_max_x < t_max_y) {
            if (t_max_x < t_max_z) {
                x += step_x;
                traveled = t_max_x;
                t_max_x += t_delta_x;
            } else {
                z += step_z;
                traveled = t_max_z;
                t_max_z += t_delta_z;
            }
        } else {
            if (t_max_y < t_max_z) {
                y += step_y;
                traveled = t_max_y;
                t_max_y += t_delta_y;
            } else {
                z += step_z;
                traveled = t_max_z;
                t_max_z += t_delta_z;
            }
        }
    }

    return result;
}
