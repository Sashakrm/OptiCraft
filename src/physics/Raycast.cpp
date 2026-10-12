#include "physics/Raycast.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Types.h"
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
        if (block != Block_Types::Air && block != Block_Types::Water && targetable) {
            result.hit = true;
            result.block_x = x;
            result.block_y = y;
            result.block_z = z;
            result.place_x = prev_x;
            result.place_y = prev_y;
            result.place_z = prev_z;
            result.distance = traveled;
            result.block_type = block;
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
