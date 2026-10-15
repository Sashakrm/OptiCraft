#include "physics/Liquid_Physics.h"

#include "utils/Config.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Types.h"

#include <algorithm>
#include <cmath>

namespace Liquid_Physics {

    namespace {
        constexpr glm::ivec3 k_horizontal[4] = {{0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}};

        bool is_liquid(Block_Types type) {
            return get_block_props(type).state == Block_State::Liquid;
        }

        // Уровень жидкости клетки: источник и падающая вода — 8, течение — 8 - level, не жидкость — 0.
        int fluid_amount(const Chunk_Manager& chunks, int x, int y, int z) {
            const Block_Types type = chunks.get_block_world(x, y, z);
            if (!is_liquid(type)) return 0;
            const uint8_t meta = chunks.get_block_meta_world(x, y, z);
            if (meta & Block_Meta::fluid_falling_bit) return 8;
            const int level = meta & Block_Meta::fluid_level_mask;
            return level == 0 ? 8 : 8 - level;
        }
    }

    Sample sample(const Chunk_Manager& chunks, const glm::vec3& feet, float height) {
        Sample s;
        const int x = static_cast<int>(std::floor(feet.x));
        const int z = static_cast<int>(std::floor(feet.z));
        const int body_y = static_cast<int>(std::floor(feet.y + height * 0.5f));
        const int head_y = static_cast<int>(std::floor(feet.y + height * 0.9f));

        s.body_in_liquid = is_liquid(chunks.get_block_world(x, body_y, z));
        s.head_in_liquid = is_liquid(chunks.get_block_world(x, head_y, z));
        if (!s.body_in_liquid) return s;

        // Течение: тянет к соседним клеткам жидкости с меньшим уровнем (только между жидкостями).
        const int here = fluid_amount(chunks, x, body_y, z);
        glm::vec3 flow(0.0f);
        for (const glm::ivec3& d : k_horizontal) {
            const int there_amount = is_liquid(chunks.get_block_world(x + d.x, body_y, z + d.z))
                ? fluid_amount(chunks, x + d.x, body_y, z + d.z)
                : -1;
            if (there_amount < 0) continue;
            flow += glm::vec3(static_cast<float>(d.x), 0.0f, static_cast<float>(d.z)) *
                    static_cast<float>(here - there_amount);
        }
        s.flow = flow * (Config::water_flow_speed / 8.0f);
        return s;
    }

    float swim_vertical_velocity(const Sample& s, bool can_swim, bool wants_up, float vertical_velocity, float dt,
                                 bool wants_down, bool sink_when_idle) {
        (void)s;
        // Сглаживание к целевой скорости (не резкий скачок), чтобы всплытие и погружение были плавными.
        const float blend = 1.0f - std::exp(-8.0f * dt);
        float target;
        if (wants_up) {
            // Удерживаемый прыжок даёт активный гребок; значение выше обычного прыжкового
            // ускорения воды позволяет выйти на поверхность и выпрыгнуть наружу.
            target = Config::water_swim_up_speed;
        } else if (wants_down) {
            target = Config::water_sink_speed;
        } else if (can_swim) {
            // Обычные плавающие мобы удерживают высоту; для игрока можно включить
            // лёгкое погружение, чтобы без гребков на него продолжала действовать вода.
            target = sink_when_idle ? Config::water_sink_speed * 0.35f : 0.0f;
        } else {
            target = Config::water_sink_speed;
        }
        return vertical_velocity + (target - vertical_velocity) * blend;
    }

    float horizontal_drag(const Sample& s) {
        return s.body_in_liquid ? Config::water_horizontal_drag : 1.0f;
    }

    int Breath::tick(float dt, bool head_in_liquid, float max_air) {
        if (head_in_liquid) {
            air = std::max(0.0f, air - dt);
        } else {
            air = std::min(max_air, air + dt * Config::breath_refill_per_second);
            drown_timer = 0.0f;
            return 0;
        }
        if (air > 0.0f) {
            drown_timer = 0.0f;
            return 0;
        }
        drown_timer += dt;
        int events = 0;
        while (drown_timer >= Config::drown_damage_interval) {
            drown_timer -= Config::drown_damage_interval;
            ++events;
        }
        return events;
    }

} // namespace Liquid_Physics
