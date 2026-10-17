//
// Created by noktemor on 11.02.2026.
//

#include "entities/player/Player.h"
#include "utils/Config.h"
#include "physics/Liquid_Physics.h"
#include "world/blocks/Block_Types.h"
#include <cmath>
#include <algorithm>

Player::Player()
    : m_position(0.0f, 10.0f, 0.0f)
    , m_velocity(0.0f)
    , m_size(Config::player_width, Config::player_height, Config::player_width)
    , m_on_ground(false)
    , m_eye_height(Config::player_eye_height)
    , m_move_speed(Config::player_move_speed)
    , m_jump_force(Config::player_jump_force)
    , m_gravity(Config::player_gravity)
    , m_view_forward(0.0f, 0.0f, -1.0f)
    , m_view_right(1.0f, 0.0f, 0.0f)
    , m_standing_height(Config::player_height)
    , m_standing_eye_height(Config::player_eye_height)
    , m_health(Config::player_max_health)
    , m_max_health(Config::player_max_health)
{}

void Player::update(float delta_time, const Chunk_Manager& chunk_manager) {
    if (m_is_riding) {
        // Сидит в седле: позицию выставляет лошадь, собственная физика не нужна.
        m_velocity = glm::vec3(0.0f);
        m_on_ground = true;
        m_fall_start_height = m_position.y;
        m_pending_input = {0.0f, 0.0f, false, false};
        return;
    }

    const glm::vec3 frame_start_position = m_position;
    glm::vec3 forward(m_view_forward.x, 0.0f, m_view_forward.z);
    glm::vec3 right(m_view_right.x, 0.0f, m_view_right.z);

    if (glm::length(forward) > 0.001f) {
        forward = glm::normalize(forward);
    }
    if (glm::length(right) > 0.001f) {
        right = glm::normalize(right);
    }
    // В полёте присед не меняет рост — Shift там используется для спуска.
    if (!m_is_flying) {
        update_crouch(chunk_manager);
    }

    // Вода: тело тормозит и сносится течением; в полёте воду игнорируем.
    const Liquid_Physics::Sample wet = m_is_flying
        ? Liquid_Physics::Sample{}
        : Liquid_Physics::sample(chunk_manager, m_position, m_size.y);
    m_in_liquid = wet.body_in_liquid;
    tick_breath(delta_time, wet.head_in_liquid);

    // Shift на земле: медленная ходьба (подкрадывание). В полёте Shift — спуск, скорость не трогаем.
    const bool sneaking = !m_is_flying && m_pending_input.crouch;
    // Бег: Ctrl на земле при движении вперёд; при приседе и в полёте не работает.
    const bool sprinting = !m_is_flying && !sneaking && m_pending_input.sprint && m_pending_input.forward > 0.0f;
    const float current_speed = (m_is_flying ? m_fly_speed
    : (sneaking ? m_move_speed * Config::player_sneak_speed_multiplier
           : (sprinting ? m_move_speed * Config::player_sprint_speed_multiplier : m_move_speed)))
        * Liquid_Physics::horizontal_drag(wet);

    if (m_pending_input.forward != 0.0f || m_pending_input.strafe != 0.0f) {
        glm::vec3 move_direction = forward * m_pending_input.forward + right * m_pending_input.strafe;
        if (glm::length(move_direction) > 0.001f) {
            move_direction = glm::normalize(move_direction);
            m_velocity.x = move_direction.x * current_speed;
            m_velocity.z = move_direction.z * current_speed;
        }
    } else {
        m_velocity.x = 0.0f;
        m_velocity.z = 0.0f;
    }
    // Импульс от инструмента и течение воды прибавляются поверх движения игрока.
    m_velocity.x += m_impulse.x + wet.flow.x;
    m_velocity.z += m_impulse.z + wet.flow.z;
    m_impulse *= std::exp(-Config::impulse_drag * delta_time);
    if (glm::length(m_impulse) < 0.02f) m_impulse = glm::vec3(0.0f);
    // Потолок горизонтальной скорости (движение + импульсы инструментов + течение воды).
    if (const float horizontal = glm::length(glm::vec2(m_velocity.x, m_velocity.z));
        horizontal > Config::player_max_speed) {
        const float k = Config::player_max_speed / horizontal;
        m_velocity.x *= k;
        m_velocity.z *= k;
        m_impulse.x *= k;
        m_impulse.z *= k;
    }

    if (m_is_flying) {
        if (m_pending_input.jump) {
            m_velocity.y = m_fly_speed;
        } else if (m_pending_input.crouch) {
            m_velocity.y = -m_fly_speed;
        } else {
            m_velocity.y = 0.0f;
        }

        m_on_ground = false;
        if (m_noclip) {
            m_position += m_velocity * delta_time;
        } else {
            move_flying_with_collision(delta_time, chunk_manager);
        }

        m_pending_input = {0.0f, 0.0f, false, false};   // <-- не забудьте 4-й false
        return;
    }
    // Отслеживание падения (урон при приземлении — см. ниже применение apply_fall_damage()).
    // "Было ли на земле" смотрим ДО сброса m_on_ground этим кадром — иначе прыжок или шаг
    // с края всегда выглядел бы как "уже в воздухе". Пока падение продолжается,
    // m_fall_start_height держит наивысшую точку с начала падения (на случай, если игрок
    // ещё поднимается после прыжка, прежде чем начать падать).
    const bool was_on_ground = m_on_ground;
    if (was_on_ground) {
        m_fall_start_height = m_position.y;
    } else {
        m_fall_start_height = std::max(m_fall_start_height, m_position.y);
    }

    // Прыжок. В воде прыжок — гребок вверх, а не отскок от земли.
    if (m_pending_input.jump && m_on_ground && !wet.body_in_liquid) {
        m_velocity.y = m_jump_force;
        m_on_ground = false;
        add_exhaustion(Config::exhaustion_jump);
    }

    if (wet.body_in_liquid) {
        if (m_pending_input.jump) {
            // Не сглаживаем гребок: на поверхности скорость иначе успевала упасть до выхода
            // центра тела из жидкого блока, и игрок снова проваливался в воду.
            m_velocity.y = std::max(m_velocity.y,
                                    std::max(Config::water_swim_up_speed, m_jump_force));
        } else {
            m_velocity.y = Liquid_Physics::swim_vertical_velocity(wet, true, false,
                m_velocity.y, delta_time, m_pending_input.crouch, true);
        }
    } else {
        m_velocity.y += m_gravity * delta_time;
    }
    m_on_ground = false;

    glm::vec3 target = m_position;

    // Подкрадывание: пока стоим на земле, шаг, после которого под ногами пусто, отменяется —
    // игрок не падает с края блока (как в Minecraft). Прыжок/падение в воздухе не ограничиваем.
    const bool edge_guard = sneaking && was_on_ground && m_velocity.y <= 0.0f;
    auto leaves_ground = [&](const glm::vec3& p) {
        return edge_guard &&
               !collides_at(p - glm::vec3(0.0f, Config::player_sneak_edge_drop, 0.0f), chunk_manager);
    };

    target.x += m_velocity.x * delta_time;
    if (collides_at(target, chunk_manager) || leaves_ground(target)) {
        target.x = m_position.x;
        m_velocity.x = 0.0f;
        m_impulse.x = 0.0f;
    } else {
        m_position.x = target.x;
    }

    target = m_position;
    target.y += m_velocity.y * delta_time;
    if (collides_at(target, chunk_manager)) {
        if (m_velocity.y < 0.0f) {
            m_on_ground = true;
            // Приземлились этим кадром (были в воздухе) — считаем урон от падения.
            if (m_fall_damage_enabled && !was_on_ground) {
                apply_fall_damage();
            }
        }
        target.y = m_position.y;
        m_velocity.y = 0.0f;
    } else {
        m_position.y = target.y;
    }

    target = m_position;
    target.z += m_velocity.z * delta_time;
    if (collides_at(target, chunk_manager) || leaves_ground(target)) {
        target.z = m_position.z;
        m_velocity.z = 0.0f;
        m_impulse.z = 0.0f;
    } else {
        m_position.z = target.z;
    }

    // Ходьба по земле тоже понемногу утомляет (0.01 за метр), бег — заметно сильнее (0.1, как в Minecraft).
    if (m_on_ground) {
        const float dx = m_position.x - frame_start_position.x;
        const float dz = m_position.z - frame_start_position.z;
        add_exhaustion(std::sqrt(dx * dx + dz * dz) *
            (sprinting ? Config::exhaustion_sprint_per_meter : Config::exhaustion_walk_per_meter));
    }

    // Сбрасываем ввод
    m_pending_input = {0.0f, 0.0f, false, false};
}

void Player::apply_impulse(const glm::vec3& direction, float speed) {
    if (speed <= 0.0f || glm::length(direction) < 0.001f) return;
    const glm::vec3 d = glm::normalize(direction);
    m_impulse += glm::vec3(d.x, 0.0f, d.z) * speed;
    m_velocity.y += d.y * speed;   // вертикальная часть — сразу в скорость (вверх или вниз)
    m_on_ground = false;
}

void Player::move_flying_with_collision(float delta_time, const Chunk_Manager& chunk_manager) {
    // Оси по очереди, как при ходьбе: упёрся в блок — стоим по этой оси и гасим скорость
    // и импульс, поэтому полёт скользит вдоль стен и не проходит сквозь них.
    for (int axis = 0; axis < 3; ++axis) {
        const float step = m_velocity[axis] * delta_time;
        if (step == 0.0f) continue;
        glm::vec3 target = m_position;
        target[axis] += step;
        if (collides_at(target, chunk_manager)) {
            m_velocity[axis] = 0.0f;
            m_impulse[axis] = 0.0f;
        } else {
            m_position = target;
        }
    }
}

void Player::tick_breath(float delta_time, bool head_in_liquid) {
    m_head_underwater = head_in_liquid;
    // Воздух тратится и утопление бьёт только по правилам Survival (см. m_fall_damage_enabled).
    if (!m_fall_damage_enabled) {
        m_breath = Liquid_Physics::Breath{};
        return;
    }
    const int events = m_breath.tick(delta_time, head_in_liquid, Config::breath_max_seconds);
    for (int i = 0; i < events; ++i) {
        take_damage(static_cast<int>(Config::drown_damage));
    }
}

void Player::set_movement_input(const Input_Manager::Move_Input& input,
                                const glm::vec3& view_forward,
                                const glm::vec3& view_right) {
    m_pending_input = input;
    m_view_forward = view_forward;
    m_view_right = view_right;
}

bool Player::check_collision(const glm::vec3& new_pos, const Chunk_Manager& chunk_manager) const {
    return collides_at(new_pos, chunk_manager);
}

bool Player::collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const {
    constexpr float epsilon = 0.001f;
    const float half_width = m_size.x * 0.5f;

    const int min_x = static_cast<int>(std::floor(position.x - half_width + epsilon));
    const int max_x = static_cast<int>(std::floor(position.x + half_width - epsilon));
    const int min_y = static_cast<int>(std::floor(position.y + epsilon));
    const int max_y = static_cast<int>(std::floor(position.y + m_size.y - epsilon));
    const int min_z = static_cast<int>(std::floor(position.z - half_width + epsilon));
    const int max_z = static_cast<int>(std::floor(position.z + half_width - epsilon));

    for (int y = min_y; y <= max_y; ++y) {
        for (int z = min_z; z <= max_z; ++z) {
            for (int x = min_x; x <= max_x; ++x) {
                if (is_solid_block_at(glm::vec3(x, y, z), chunk_manager)) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool Player::is_solid_block_at(const glm::vec3& world_pos, const Chunk_Manager& chunk_manager) const {
    const int wx = static_cast<int>(std::floor(world_pos.x));
    const int wy = static_cast<int>(std::floor(world_pos.y));
    const int wz = static_cast<int>(std::floor(world_pos.z));
    Block_Types type = chunk_manager.get_block_world(wx, wy, wz);
    return get_block_props(type).is_solid;
}

bool Player::intersects_block_aabb(int block_x, int block_y, int block_z) const {
    const float half_width = m_size.x * 0.5f;
    const glm::vec3 player_min(m_position.x - half_width, m_position.y, m_position.z - half_width);
    const glm::vec3 player_max(m_position.x + half_width, m_position.y + m_size.y, m_position.z + half_width);
    const glm::vec3 block_min(static_cast<float>(block_x), static_cast<float>(block_y), static_cast<float>(block_z));
    const glm::vec3 block_max = block_min + glm::vec3(1.0f);

    return player_min.x < block_max.x && player_max.x > block_min.x &&
           player_min.y < block_max.y && player_max.y > block_min.y &&
           player_min.z < block_max.z && player_max.z > block_min.z;
}

void Player::update_crouch(const Chunk_Manager& chunk_manager) {
    const bool wants_crouch = m_pending_input.crouch;
    const float target_height = wants_crouch ? Config::player_crouch_height : m_standing_height;

    if (target_height < m_size.y) {
        // Уменьшать хитбокс всегда безопасно — ноги (position.y) не двигаются.
        m_size.y = target_height;
        m_is_crouching = true;
    } else if (target_height > m_size.y) {
        // Пытаемся встать — только если сверху не блок.
        if (would_fit_at_height(target_height, chunk_manager)) {
            m_size.y = target_height;
            m_is_crouching = wants_crouch;
        }
        // Иначе остаёмся присевшими ещё на кадр.
    }

    m_eye_height = m_standing_eye_height * (m_size.y / m_standing_height);
}

bool Player::would_fit_at_height(float height, const Chunk_Manager& chunk_manager) const {
    constexpr float epsilon = 0.001f;
    const float half_width = m_size.x * 0.5f;

    const int min_x = static_cast<int>(std::floor(m_position.x - half_width + epsilon));
    const int max_x = static_cast<int>(std::floor(m_position.x + half_width - epsilon));
    const int min_y = static_cast<int>(std::floor(m_position.y + epsilon));
    const int max_y = static_cast<int>(std::floor(m_position.y + height - epsilon));
    const int min_z = static_cast<int>(std::floor(m_position.z - half_width + epsilon));
    const int max_z = static_cast<int>(std::floor(m_position.z + half_width - epsilon));

    for (int y = min_y; y <= max_y; ++y) {
        for (int z = min_z; z <= max_z; ++z) {
            for (int x = min_x; x <= max_x; ++x) {
                if (is_solid_block_at(glm::vec3(x, y, z), chunk_manager)) {
                    return false;
                }
            }
        }
    }
    return true;
}

void Player::take_damage(int amount) {
    if (amount <= 0 || !is_alive()) return;
    m_health = std::max(0, m_health - amount);
    add_exhaustion(Config::exhaustion_damage);
}

void Player::heal(int amount) {
    if (amount <= 0) return;
    m_health = std::min(m_max_health, m_health + amount);
}

void Player::apply_fall_damage() {
    const float fall_distance = m_fall_start_height - m_position.y;
    if (fall_distance <= Config::fall_damage_safe_height) return;

    const int damage = static_cast<int>(std::floor(
        (fall_distance - Config::fall_damage_safe_height) * Config::fall_damage_per_block));
    if (damage > 0) take_damage(damage);
}

void Player::respawn(const glm::vec3& position) {
    m_position = position;
    m_velocity = glm::vec3(0.0f);
    m_health = m_max_health;
    m_fall_start_height = position.y;
    // После смерти голод тоже начинается заново.
    m_food_level = Config::player_max_food;
    m_saturation = Config::player_initial_saturation;
    m_exhaustion = 0.0f;
    m_hunger_regen_timer = 0.0f;
    m_heal_fraction = 0.0f;
    m_starvation_timer = 0.0f;
    m_impulse = glm::vec3(0.0f);
    m_breath = Liquid_Physics::Breath{};
}

void Player::add_exhaustion(float amount) {
    if (!m_hunger_enabled || amount <= 0.0f) return;
    m_exhaustion += amount;
}

void Player::eat(int food_points, float saturation) {
    m_food_level = std::min(Config::player_max_food, m_food_level + std::max(0, food_points));
    m_saturation = std::min(static_cast<float>(m_food_level), m_saturation + std::max(0.0f, saturation));
}

void Player::update_hunger(float delta_time) {
    if (!m_hunger_enabled || !is_alive()) return;

    // 1) Истощение -> сначала тратится насыщение, потом сама еда.
    while (m_exhaustion >= Config::hunger_exhaustion_threshold) {
        m_exhaustion -= Config::hunger_exhaustion_threshold;
        if (m_saturation > 0.0f) {
            m_saturation = std::max(0.0f, m_saturation - 1.0f);
        } else {
            m_food_level = std::max(0, m_food_level - 1);
        }
    }
    // Насыщение не может превышать текущую еду (иначе после голода осталась бы "невидимая" сытость).
    m_saturation = std::min(m_saturation, static_cast<float>(m_food_level));

    // 2) Естественное лечение зависит от сытости (см. Config::hunger_*):
    //    полная еда + насыщение -> быстро, еда >= порога -> медленно, иначе не лечимся.
    const bool wounded = m_health < m_max_health;
    const bool fast_regen = m_food_level >= Config::player_max_food && m_saturation > 0.0f;
    const bool slow_regen = !fast_regen && m_food_level >= Config::hunger_regen_min_food;
    if (wounded && (fast_regen || slow_regen)) {
        const float interval = fast_regen ? Config::hunger_fast_regen_interval : Config::hunger_regen_interval;
        m_hunger_regen_timer += delta_time;
        if (m_hunger_regen_timer >= interval) {
            m_hunger_regen_timer = 0.0f;
            if (fast_regen) {
                const float spent = std::min(m_saturation, Config::hunger_fast_regen_cap);
                m_heal_fraction += spent / Config::hunger_fast_regen_cap;
                add_exhaustion(spent);
            } else {
                m_heal_fraction += 1.0f;
                add_exhaustion(Config::exhaustion_regen);
            }
            const int whole = static_cast<int>(m_heal_fraction);
            if (whole > 0) {
                m_heal_fraction -= static_cast<float>(whole);
                heal(whole);
            }
        }
    } else {
        m_hunger_regen_timer = 0.0f;
        if (!wounded) m_heal_fraction = 0.0f;
    }

    // 3) Полностью голоден — раз в интервал 1 HP урона (не ниже Config::starvation_health_floor).
    if (m_food_level <= 0) {
        m_starvation_timer += delta_time;
        if (m_starvation_timer >= Config::starvation_interval) {
            m_starvation_timer = 0.0f;
            if (m_health > Config::starvation_health_floor) {
                take_damage(1);
            }
        }
    } else {
        m_starvation_timer = 0.0f;
    }
}
