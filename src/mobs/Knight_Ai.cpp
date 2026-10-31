#include "mobs/Knight_Ai.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Types.h"
#include "utils/Config.h"
#include "rendering/Shader.h"
#include "utils/logging/Logger.h"

namespace {
constexpr float kPi = glm::pi<float>();
constexpr float kTwoPi = glm::two_pi<float>();
constexpr float kHurtSeconds = 0.42f;
constexpr float kDegToRad = kPi / 180.0f;

float wrap_angle(float angle) {
    while (angle > kPi) angle -= kTwoPi;
    while (angle < -kPi) angle += kTwoPi;
    return angle;
}

float angle_delta(float from, float to) {
    return wrap_angle(to - from);
}

float horizontal_distance(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(glm::vec2(a.x - b.x, a.z - b.z));
}

glm::vec3 forward_from_yaw(float yaw) {
    return {std::sin(yaw), 0.0f, std::cos(yaw)};
}

bool solid_block(const Chunk_Manager& chunks, int x, int y, int z) {
    if (y < Config::world_min_y || y > Config::world_max_y) return true;
    const Block_Types block = chunks.get_block_world(x, y, z);
    return get_block_props(block).is_solid;
}

struct Attack_Properties {
    float range;
    float damage;
    float windup;
    float active;
    float recovery;
    float cooldown;
};

Attack_Properties attack_properties(Knight_Attack attack) {
    switch (attack) {
        case Knight_Attack::Swipe:        return {5.6f, 19.0f, 0.62f, 0.32f, 0.72f, 1.35f};
        case Knight_Attack::Double_Swipe: return {5.8f, 14.0f, 0.75f, 0.76f, 0.82f, 2.7f};
        case Knight_Attack::Ground_Slam:  return {8.5f, 29.0f, 1.05f, 0.35f, 1.10f, 4.4f};
        case Knight_Attack::Charge:       return {19.0f, 28.0f, 0.72f, 0.95f, 1.05f, 4.8f};
        case Knight_Attack::Leap_Strike:  return {23.0f, 34.0f, 0.92f, 1.1f, 1.12f, 5.4f};
        case Knight_Attack::Shockwave:    return {16.0f, 32.0f, 1.22f, 0.36f, 1.25f, 6.0f};
        case Knight_Attack::Spin_Attack:  return {8.0f, 13.0f, 0.82f, 1.10f, 0.95f, 4.2f};
        case Knight_Attack::Rage_Strike:  return {10.5f, 43.0f, 1.10f, 0.40f, 1.35f, 6.8f};
        default:                          return {0, 0, 0, 0, 0, 0};
    }
}

const char* attack_animation(Knight_Attack attack) {
    switch (attack) {
        case Knight_Attack::Swipe: return "Attack_Slash";
        case Knight_Attack::Double_Swipe: return "Attack_Double";
        case Knight_Attack::Ground_Slam:
        case Knight_Attack::Shockwave:
        case Knight_Attack::Rage_Strike: return "Attack_Slam";
        case Knight_Attack::Charge: return "Charge";
        case Knight_Attack::Leap_Strike: return "Jump";
        case Knight_Attack::Spin_Attack: return "Spin";
        default: return "Idle";
    }
}
}

bool Knight_Ai::Arena::contains(const glm::vec3& p, float margin) const {
    const float rx = std::max(1.0f, half_x - margin);
    const float rz = std::max(1.0f, half_z - margin);
    const float dx = std::abs(p.x - center.x);
    const float dz = std::abs(p.z - center.z);
    if (dx > rx || dz > rz || p.y < floor_y - 0.01f || p.y > floor_y + ARENA_HEIGHT - BODY_HEIGHT)
        return false;

    const float cut = std::min(corner_cut, std::min(rx, rz) * 0.45f);
    if (dx > rx - cut && dz > rz - cut) {
        const float nx = (dx - (rx - cut)) / cut;
        const float nz = (dz - (rz - cut)) / cut;
        if (nx * nx + nz * nz > 1.0f) return false;
    }
    return true;
}

glm::vec3 Knight_Ai::Arena::clamp(const glm::vec3& input, float margin) const {
    glm::vec3 p = input;
    const float rx = std::max(1.0f, half_x - margin);
    const float rz = std::max(1.0f, half_z - margin);
    p.x = glm::clamp(p.x, center.x - rx, center.x + rx);
    p.z = glm::clamp(p.z, center.z - rz, center.z + rz);
    p.y = glm::clamp(p.y, floor_y, floor_y + ARENA_HEIGHT - BODY_HEIGHT);

    const float cut = std::min(corner_cut, std::min(rx, rz) * 0.45f);
    const float dx = std::abs(p.x - center.x);
    const float dz = std::abs(p.z - center.z);
    if (dx > rx - cut && dz > rz - cut) {
        const float ox = dx - (rx - cut);
        const float oz = dz - (rz - cut);
        const float len = std::sqrt(ox * ox + oz * oz);
        if (len > cut && len > 0.0001f) {
            const float scale = cut / len;
            const float sign_x = p.x >= center.x ? 1.0f : -1.0f;
            const float sign_z = p.z >= center.z ? 1.0f : -1.0f;
            p.x = center.x + sign_x * (rx - cut + ox * scale);
            p.z = center.z + sign_z * (rz - cut + oz * scale);
        }
    }
    return p;
}

Knight_Ai::Knight_Ai(std::string name, const glm::vec3& spawn_position)
    : m_name(std::move(name)), m_position(spawn_position), m_last_seen_position(spawn_position) {
    m_arena.center = spawn_position;
    m_arena.floor_y = spawn_position.y;
    m_position = m_arena.clamp(spawn_position, 1.5f);
}

void Knight_Ai::face_towards(const glm::vec3& target) {
    glm::vec3 direction = target - m_position;
    direction.y = 0.0f;
    if (glm::dot(direction, direction) < 0.0001f) return;
    m_yaw = std::atan2(direction.x, direction.z);
    m_model.set_yaw_degrees(glm::degrees(m_yaw));
}

bool Knight_Ai::load_model() {
    if (!m_model.load(MODEL_PATH)) {
        LOG_ERROR(std::string("Knight_Ai: required model could not be loaded: ") + MODEL_PATH);
        return false;
    }
    if (!m_model.has_skeleton()) {
        LOG_ERROR(std::string("Knight_Ai: required model has no skeletal rig: ") + MODEL_PATH);
        return false;
    }
    static constexpr const char* required_animations[] = {
        "Idle", "Walk", "Attack_Slash", "Attack_Double", "Attack_Slam",
        "Jump", "Charge", "Spin", "Hit", "Death"
    };
    for (const char* animation : required_animations) {
        if (!m_model.has_animation(animation)) {
            LOG_ERROR(std::string("Knight_Ai: required animation '") + animation + "' is missing from " + MODEL_PATH);
            return false;
        }
    }
    m_model.set_position(m_position);
    m_model.set_scale(1.0f);
    m_model.set_yaw_degrees(glm::degrees(m_yaw));
    set_animation("Idle", true);
    return true;
}

bool Knight_Ai::has_line_of_sight(const Knight_Player_View& player, const Chunk_Manager& chunks) const {
    if (!player.alive) return false;

    const glm::vec3 from = m_position + glm::vec3(0.0f, BODY_HEIGHT * 0.72f, 0.0f);
    const glm::vec3 to = player.position + glm::vec3(0.0f, player.eye_height * 0.82f, 0.0f);
    const glm::vec3 delta = to - from;
    const float distance = glm::length(delta);
    if (distance < 0.01f) return true;

    // Отдельный тест зрения, а не Raycast прицеливания игрока: Raycast также
    // останавливается на траве/цветах и мелких targetable-объектах. Они не должны
    // полностью ослеплять босса. Для зрения препятствием является непрозрачный блок.
    constexpr float kVisionStep = 0.20f;
    const int steps = std::max(1, static_cast<int>(std::ceil(distance / kVisionStep)));
    for (int i = 1; i < steps; ++i) {
        const glm::vec3 sample = from + delta * (static_cast<float>(i) / static_cast<float>(steps));
        const int x = static_cast<int>(std::floor(sample.x));
        const int y = static_cast<int>(std::floor(sample.y));
        const int z = static_cast<int>(std::floor(sample.z));
        if (y < Config::world_min_y || y > Config::world_max_y) return false;

        const Block_Types block = chunks.get_block_world(x, y, z);
        const Block_Properties& props = get_block_props(block);
        if (props.is_opaque) return false;
    }
    return true;
}

bool Knight_Ai::can_see_player(const Knight_Player_View& player, const Chunk_Manager& chunks) const {
    if (!player.alive) return false;
    const float distance = glm::distance(m_position + glm::vec3(0, 1.5f, 0), player.position);
    const float range = (m_phase == 1 ? 48.0f : (m_phase == 2 ? 56.0f : 64.0f));
    if (distance > range || !has_line_of_sight(player, chunks)) return false;

    glm::vec3 direction = player.position - m_position;
    direction.y = 0.0f;
    if (glm::dot(direction, direction) < 0.0001f) return true;
    direction = glm::normalize(direction);
    const float half_fov = (m_phase == 1 ? 75.0f : (m_phase == 2 ? 87.0f : 100.0f)) * kDegToRad;
    const float dot = glm::clamp(glm::dot(forward_from_yaw(m_yaw), direction), -1.0f, 1.0f);
    return std::acos(dot) <= half_fov;
}

bool Knight_Ai::can_occupy(const glm::vec3& feet, const Chunk_Manager& chunks) const {
    const glm::vec3 lo(feet.x - BODY_HALF_X, feet.y + 0.05f, feet.z - BODY_HALF_Z);
    const glm::vec3 hi(feet.x + BODY_HALF_X, feet.y + BODY_HEIGHT - 0.05f, feet.z + BODY_HALF_Z);
    const int min_x = static_cast<int>(std::floor(lo.x));
    const int max_x = static_cast<int>(std::floor(hi.x));
    const int min_y = static_cast<int>(std::floor(lo.y));
    const int max_y = static_cast<int>(std::floor(hi.y));
    const int min_z = static_cast<int>(std::floor(lo.z));
    const int max_z = static_cast<int>(std::floor(hi.z));
    for (int x = min_x; x <= max_x; ++x)
        for (int y = min_y; y <= max_y; ++y)
            for (int z = min_z; z <= max_z; ++z)
                if (solid_block(chunks, x, y, z)) return false;
    return true;
}

bool Knight_Ai::has_ground_below(const glm::vec3& feet, const Chunk_Manager& chunks) const {
    const float y = feet.y - 0.12f;
    const float x_offset = BODY_HALF_X * 0.72f;
    const float z_offset = BODY_HALF_Z * 0.72f;
    const int samples[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    for (const auto& sample : samples) {
        const int x = static_cast<int>(std::floor(feet.x + sample[0] * x_offset));
        const int z = static_cast<int>(std::floor(feet.z + sample[1] * z_offset));
        if (solid_block(chunks, x, static_cast<int>(std::floor(y)), z)) return true;
    }
    return feet.y <= m_arena.floor_y + 0.02f;
}

void Knight_Ai::update_learning(const Knight_Player_View& player, float dt) {
    m_learning_timer += dt;
    const float decay = std::exp(-dt * 0.055f);
    m_rear_attempts *= decay;
    m_front_approaches *= decay;
    m_left_orbits *= decay;
    m_right_orbits *= decay;
    if (m_learning_timer < 0.12f) return;
    m_learning_timer = 0.0f;

    glm::vec3 direction = player.position - m_position;
    direction.y = 0.0f;
    const float distance = glm::length(direction);
    if (distance < 0.1f || distance > 24.0f) return;
    direction /= distance;
    const glm::vec3 forward = forward_from_yaw(m_yaw);
    const float facing = glm::dot(forward, direction);
    const glm::vec3 right(forward.z, 0.0f, -forward.x);
    const float side = glm::dot(right, direction);

    if (facing < -0.18f) m_rear_attempts += 1.0f;
    else if (facing > 0.45f) m_front_approaches += 1.0f;
    if (side < -0.35f) m_left_orbits += 1.0f;
    if (side > 0.35f) m_right_orbits += 1.0f;

    const float total = m_rear_attempts + m_front_approaches + 0.001f;
    m_rear_habit = m_rear_attempts / total;
    if (m_left_orbits > m_right_orbits * 1.18f) m_preferred_orbit = -1;
    else if (m_right_orbits > m_left_orbits * 1.18f) m_preferred_orbit = 1;
    else m_preferred_orbit = 0;
}

glm::vec3 Knight_Ai::predict_player(const Knight_Player_View& player, float seconds) const {
    seconds = glm::clamp(seconds, 0.0f, 1.15f);
    glm::vec3 velocity = player.velocity;
    const float speed = glm::length(velocity);
    if (speed > 12.0f) velocity *= 12.0f / speed;
    glm::vec3 predicted = player.position + velocity * seconds;
    const glm::vec3 delta = predicted - player.position;
    const float distance = glm::length(delta);
    if (distance > 10.0f) predicted = player.position + delta * (10.0f / distance);
    predicted = m_arena.clamp(predicted, 0.75f);
    return predicted;
}

void Knight_Ai::rotate_towards(const glm::vec3& target, float dt, float multiplier) {
    glm::vec3 direction = target - m_position;
    direction.y = 0.0f;
    if (glm::dot(direction, direction) < 0.0001f) return;
    const float desired = std::atan2(direction.x, direction.z);
    const float difference = angle_delta(m_yaw, desired);
    const float speed = (m_phase == 1 ? 4.2f : (m_phase == 2 ? 5.7f : 7.4f)) * multiplier;
    m_yaw = wrap_angle(m_yaw + glm::clamp(difference, -speed * dt, speed * dt));
    m_model.set_yaw_degrees(glm::degrees(m_yaw));
}

bool Knight_Ai::facing_player(const Knight_Player_View& player) const {
    glm::vec3 d = player.position - m_position;
    d.y = 0.0f;
    if (glm::dot(d, d) < 0.0001f) return true;
    d = glm::normalize(d);
    const float half_fov = (m_phase == 1 ? 75.0f : (m_phase == 2 ? 87.0f : 100.0f)) * kDegToRad;
    return glm::dot(forward_from_yaw(m_yaw), d) >= std::cos(half_fov);
}

bool Knight_Ai::can_see_at_impact(const Knight_Player_View& player, const Chunk_Manager& chunks) const {
    // Атака не наносит урон без прямой видимости и нахождения игрока в секторе обзора.
    return can_see_player(player, chunks);
}

void Knight_Ai::set_animation(const char* name, bool loop) {
    if (!m_model.is_loaded()) return;
    const std::string requested = name ? name : "Idle";
    if (!m_model.has_animation(requested)) {
        if (m_model.has_animation("Idle")) {
            if (m_current_animation != "Idle") {
                m_model.play_animation("Idle", true);
                m_current_animation = "Idle";
            }
        }
        return;
    }
    if (m_current_animation == requested) {
        if (loop) return;
        // Mob intentionally doesn't restart an already selected clip. Переключение
        // через Idle позволяет повторно запускать одинаковую атаку/Hit с нулевого кадра.
        if (requested != "Idle" && m_model.has_animation("Idle")) {
            m_model.play_animation("Idle", true);
            m_current_animation = "Idle";
        } else {
            return;
        }
    }
    m_model.play_animation(requested, loop);
    m_current_animation = requested;
}

void Knight_Ai::update_phase() {
    const float ratio = get_health_percent();
    const int next_phase = ratio <= 0.34f ? 3 : (ratio <= 0.68f ? 2 : 1);
    if (next_phase == m_phase) return;
    m_phase = next_phase;
    m_state_timer = 1.0f;
    m_attack_cooldown = std::min(m_attack_cooldown, 0.65f);
    if (m_state != Knight_State::Dead) m_state = Knight_State::Recovery;
    set_animation("Attack_Slam", false);
}

bool Knight_Ai::attack_is_available(Knight_Attack attack, float distance) const {
    if (attack == Knight_Attack::None) return false;
    const Attack_Properties info = attack_properties(attack);
    if (distance > info.range) return false;
    switch (attack) {
        case Knight_Attack::Swipe:
        case Knight_Attack::Double_Swipe:
        case Knight_Attack::Ground_Slam:
            return true;
        case Knight_Attack::Charge:
        case Knight_Attack::Leap_Strike:
            return m_phase >= 2;
        case Knight_Attack::Shockwave:
        case Knight_Attack::Spin_Attack:
        case Knight_Attack::Rage_Strike:
            return m_phase >= 3;
        default: return false;
    }
}

Knight_Attack Knight_Ai::choose_attack(const Knight_Player_View& player, const Chunk_Manager& chunks) {
    if (!can_see_player(player, chunks) || m_attack_cooldown > 0.0f) return Knight_Attack::None;
    const float distance = horizontal_distance(m_position, player.position);
    std::array<Knight_Attack, 8> candidates{};
    std::array<float, 8> weights{};
    size_t count = 0;
    for (int value = static_cast<int>(Knight_Attack::Swipe);
         value < static_cast<int>(Knight_Attack::Count); ++value) {
        const auto attack = static_cast<Knight_Attack>(value);
        if (!attack_is_available(attack, distance) || attack == m_last_attack) continue;
        const float cooldown_factor = (attack == Knight_Attack::None) ? 0.0f : 1.0f;
        float weight = 1.0f * cooldown_factor;
        switch (attack) {
            case Knight_Attack::Swipe:        weight = distance < 5.8f ? 3.0f : 0.45f; break;
            case Knight_Attack::Double_Swipe: weight = distance < 5.8f ? 2.0f : 0.35f; break;
            case Knight_Attack::Ground_Slam:  weight = distance < 8.5f ? 1.7f : 0.3f; break;
            case Knight_Attack::Charge:       weight = distance > 7.0f ? 2.8f : 0.35f; break;
            case Knight_Attack::Leap_Strike:  weight = distance > 9.0f ? 2.8f : 0.4f; break;
            case Knight_Attack::Shockwave:    weight = distance > 5.0f ? 2.0f : 1.0f; break;
            case Knight_Attack::Spin_Attack:  weight = distance < 8.0f ? 2.8f : 0.4f; break;
            case Knight_Attack::Rage_Strike:  weight = distance < 10.5f ? 2.2f : 0.35f; break;
            default: break;
        }
        if (m_rear_habit > 0.55f && (attack == Knight_Attack::Spin_Attack || attack == Knight_Attack::Ground_Slam))
            weight *= 1.65f;
        candidates[count] = attack;
        weights[count] = weight;
        ++count;
    }
    if (count == 0) return Knight_Attack::None;

    float total = 0.0f;
    for (size_t i = 0; i < count; ++i) total += weights[i];
    // Детерминированное псевдослучайное число без выделения памяти/глобального RNG.
    const float noise = std::fmod(std::abs(std::sin(m_clock * 12.9898f + m_position.x * 0.17f) * 43758.5453f), 1.0f);
    float pick = noise * total;
    for (size_t i = 0; i < count; ++i) {
        pick -= weights[i];
        if (pick <= 0.0f) return candidates[i];
    }
    return candidates[count - 1];
}

void Knight_Ai::start_attack(Knight_Attack attack, const Knight_Player_View& player) {
    if (attack == Knight_Attack::None) return;
    const Attack_Properties info = attack_properties(attack);
    m_current_attack = attack;
    m_last_attack = attack;
    m_attack_aim = predict_player(player, info.windup);
    m_state = Knight_State::Windup;
    m_state_timer = info.windup;
    m_hit_primary = m_hit_secondary = m_hit_tertiary = false;
    set_animation(attack_animation(attack), false);
}

void Knight_Ai::move_towards(const glm::vec3& target, float speed, float dt, const Chunk_Manager& chunks) {
    glm::vec3 delta = target - m_position;
    delta.y = 0.0f;
    const float distance = glm::length(delta);
    if (distance < 0.12f) return;
    const glm::vec3 direction = delta / distance;
    rotate_towards(target, dt, 1.0f + (m_rear_habit > 0.45f ? 0.8f : 0.0f));

    glm::vec3 step = direction * (speed * dt);
    if (distance < glm::length(step)) step = direction * distance;
    glm::vec3 candidate = m_arena.clamp(m_position + step, 1.5f);
    if (can_occupy(candidate, chunks)) {
        m_position = candidate;
        m_grounded = has_ground_below(m_position, chunks);
        if (!m_grounded) m_velocity.y = 0.0f;
        return;
    }

    // Рыцарь способен перешагивать блоки и невысокие уступы, не застревая
    // на каждом перепаде рельефа.
    for (float rise = 0.5f; rise <= 1.5f; rise += 0.5f) {
        glm::vec3 stepped = candidate;
        stepped.y = m_position.y + rise;
        if (can_occupy(stepped, chunks) && has_ground_below(stepped, chunks)) {
            m_position = stepped;
            m_grounded = true;
            m_velocity.y = 0.0f;
            return;
        }
    }

    // Простое локальное обходное движение: пробуем оба фланга, выбираем свободный.
    const glm::vec3 left(-direction.z, 0.0f, direction.x);
    const glm::vec3 right(direction.z, 0.0f, -direction.x);
    const glm::vec3 candidates[2] = {
        m_arena.clamp(m_position + (direction * 0.35f + left * 0.94f) * (speed * dt), 1.5f),
        m_arena.clamp(m_position + (direction * 0.35f + right * 0.94f) * (speed * dt), 1.5f)
    };
    const bool left_ok = can_occupy(candidates[0], chunks);
    const bool right_ok = can_occupy(candidates[1], chunks);
    if (left_ok || right_ok) {
        if (left_ok && right_ok) {
            const float left_d = horizontal_distance(candidates[0], target);
            const float right_d = horizontal_distance(candidates[1], target);
            m_position = left_d <= right_d ? candidates[0] : candidates[1];
        } else {
            m_position = left_ok ? candidates[0] : candidates[1];
        }
        m_grounded = has_ground_below(m_position, chunks);
        if (!m_grounded) m_velocity.y = 0.0f;
    }
}

void Knight_Ai::begin_jump(const glm::vec3& target, int phase, bool attack_landing) {
    if (!m_grounded) return;
    glm::vec3 delta = target - m_position;
    delta.y = 0.0f;
    const float distance = glm::length(delta);
    if (distance < 1.5f) return;
    const glm::vec3 direction = delta / distance;
    const float speed = (phase == 1 ? 26.0f : (phase == 2 ? 31.0f : 36.0f)) * m_effects.speed_factor();
    m_velocity.x = direction.x * std::min(speed, distance * 0.95f);
    m_velocity.z = direction.z * std::min(speed, distance * 0.95f);
    m_velocity.y = (phase == 1 ? 18.0f : (phase == 2 ? 21.0f : 24.0f)) * m_effects.jump_factor();
    m_grounded = false;
    m_pending_leap_impact = attack_landing;
    m_state = Knight_State::Jumping;
    m_state_timer = 0.0f;
    m_jump_cooldown = phase == 1 ? 2.0f : (phase == 2 ? 1.45f : 1.0f);
    set_animation("Jump", false);
}

void Knight_Ai::update_jump(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks, Knight_Tick_Result& result) {
    m_velocity.y -= GRAVITY * dt;
    glm::vec3 next = m_arena.clamp(m_position + glm::vec3(m_velocity.x * dt, 0.0f, m_velocity.z * dt), 1.5f);
    if (can_occupy(next, chunks)) m_position.x = next.x, m_position.z = next.z;
    else m_velocity.x *= -0.18f, m_velocity.z *= -0.18f;

    const float next_y = m_position.y + m_velocity.y * dt;
    glm::vec3 vertical = m_position;
    vertical.y = next_y;
    vertical = m_arena.clamp(vertical, 1.5f);
    if (can_occupy(vertical, chunks)) {
        m_position.y = vertical.y;
    } else if (m_velocity.y < 0.0f) {
        m_grounded = true;
        m_position.y = std::max(m_arena.floor_y, m_position.y);
        m_velocity.y = 0.0f;
    } else {
        m_velocity.y = 0.0f;
    }

    if (m_position.y <= m_arena.floor_y + 0.02f || (m_velocity.y <= 0.0f && has_ground_below(m_position, chunks))) {
        m_grounded = true;
        m_position.y = std::max(m_position.y, m_arena.floor_y);
        m_velocity = glm::vec3(0.0f);
        if (m_pending_leap_impact) {
            if (can_see_at_impact(player, chunks) && horizontal_distance(m_position, player.position) <= 8.0f)
                result.player_damage += 34;
            m_pending_leap_impact = false;
            m_state = Knight_State::Recovery;
            m_state_timer = attack_properties(Knight_Attack::Leap_Strike).recovery;
            m_attack_cooldown = attack_properties(Knight_Attack::Leap_Strike).cooldown;
            m_current_attack = Knight_Attack::Leap_Strike;
        } else {
            m_state = Knight_State::Chase;
        }
        set_animation("Idle", true);
    }
}

void Knight_Ai::apply_attack_hit(const Knight_Player_View& player, const Chunk_Manager& chunks,
                                 float range, float damage, float arc_degrees, Knight_Tick_Result& result) {
    if (!can_see_at_impact(player, chunks)) return;
    const float distance = horizontal_distance(m_position, player.position);
    if (distance > range) return;
    const float vertical_limit = arc_degrees >= 359.0f ? 5.0f : 3.8f;
    if (std::abs(player.position.y - m_position.y) > vertical_limit) return;
    if (arc_degrees < 359.0f) {
        glm::vec3 direction = player.position - m_position;
        direction.y = 0.0f;
        if (glm::dot(direction, direction) > 0.0001f) {
            direction = glm::normalize(direction);
            const float dot = glm::clamp(glm::dot(forward_from_yaw(m_yaw), direction), -1.0f, 1.0f);
            if (std::acos(dot) > arc_degrees * 0.5f * kDegToRad) return;
        }
    }
    result.player_damage += static_cast<int>(std::round(damage));
}

void Knight_Ai::update_windup(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks) {
    if (!can_see_player(player, chunks)) {
        m_state = Knight_State::Recovery;
        m_state_timer = 0.45f;
        m_current_attack = Knight_Attack::None;
        m_attack_cooldown = 0.6f;
        set_animation("Idle", true);
        return;
    }
    m_attack_aim = predict_player(player, m_state_timer);
    rotate_towards(m_attack_aim, dt, 1.25f);
    m_state_timer -= dt;
    if (m_state_timer > 0.0f) return;

    // Проверка сектора и препятствий выполняется вновь непосредственно перед атакой.
    if (!can_see_player(player, chunks)) {
        m_state = Knight_State::Recovery;
        m_state_timer = 0.4f;
        return;
    }
    m_state = Knight_State::Attacking;
    m_state_timer = 0.0f;
    if (m_current_attack == Knight_Attack::Leap_Strike) {
        begin_jump(predict_player(player, 0.45f), m_phase, true);
    }
}

void Knight_Ai::update_attack(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks, Knight_Tick_Result& result) {
    const Attack_Properties info = attack_properties(m_current_attack);
    m_state_timer += dt;
    const float t = m_state_timer;

    // Если игрок скрылся из поля зрения, атака прекращается: урон сквозь стены невозможен.
    if (!can_see_at_impact(player, chunks)) {
        m_state = Knight_State::Recovery;
        m_state_timer = 0.35f;
        return;
    }

    switch (m_current_attack) {
        case Knight_Attack::Swipe:
            if (!m_hit_primary && t >= 0.18f) {
                apply_attack_hit(player, chunks, 5.6f, 19.0f, 112.0f, result);
                m_hit_primary = true;
            }
            break;
        case Knight_Attack::Double_Swipe:
            if (!m_hit_primary && t >= 0.20f) {
                apply_attack_hit(player, chunks, 5.8f, 14.0f, 120.0f, result);
                m_hit_primary = true;
            }
            if (!m_hit_secondary && t >= 0.53f) {
                apply_attack_hit(player, chunks, 5.8f, 14.0f, 120.0f, result);
                m_hit_secondary = true;
            }
            break;
        case Knight_Attack::Ground_Slam:
            if (!m_hit_primary && t >= 0.24f) {
                apply_attack_hit(player, chunks, 8.5f, 29.0f, 360.0f, result);
                m_hit_primary = true;
            }
            break;
        case Knight_Attack::Charge: {
            glm::vec3 direction = predict_player(player, 0.15f) - m_position;
            direction.y = 0.0f;
            if (glm::dot(direction, direction) > 0.001f) {
                direction = glm::normalize(direction);
                const glm::vec3 next = m_arena.clamp(m_position + direction * (22.0f * dt), 1.5f);
                if (can_occupy(next, chunks)) {
                    m_position = next;
                    m_grounded = has_ground_below(m_position, chunks);
                    if (!m_grounded) m_velocity.y = 0.0f;
                }
            }
            if (!m_hit_primary && t >= 0.28f) {
                apply_attack_hit(player, chunks, 4.2f, 28.0f, 85.0f, result);
                m_hit_primary = true;
            }
            break;
        }
        case Knight_Attack::Shockwave:
            if (!m_hit_primary && t >= 0.20f) {
                apply_attack_hit(player, chunks, 16.0f, 32.0f, 360.0f, result);
                m_hit_primary = true;
            }
            break;
        case Knight_Attack::Spin_Attack:
            if (!m_hit_primary && t >= 0.18f) {
                apply_attack_hit(player, chunks, 8.0f, 13.0f, 360.0f, result);
                m_hit_primary = true;
            }
            if (!m_hit_secondary && t >= 0.52f) {
                apply_attack_hit(player, chunks, 8.0f, 13.0f, 360.0f, result);
                m_hit_secondary = true;
            }
            if (!m_hit_tertiary && t >= 0.86f) {
                apply_attack_hit(player, chunks, 8.0f, 13.0f, 360.0f, result);
                m_hit_tertiary = true;
            }
            break;
        case Knight_Attack::Rage_Strike:
            if (!m_hit_primary && t >= 0.26f) {
                apply_attack_hit(player, chunks, 10.5f, 43.0f, 100.0f, result);
                m_hit_primary = true;
            }
            break;
        case Knight_Attack::Leap_Strike:
            // Урон наносится только при приземлении в update_jump().
            break;
        default: break;
    }

    if (t >= info.active) {
        m_state = Knight_State::Recovery;
        m_state_timer = info.recovery;
        m_attack_cooldown = info.cooldown * (m_phase == 1 ? 1.0f : (m_phase == 2 ? 0.82f : 0.68f));
    }
}

void Knight_Ai::update_recovery(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks) {
    m_state_timer -= dt;
    if (m_state_timer > 0.0f) return;
    m_current_attack = Knight_Attack::None;
    if (can_see_player(player, chunks)) m_state = Knight_State::Chase;
    else m_state = Knight_State::Returning;
    set_animation("Idle", true);
}

void Knight_Ai::update_movement(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks) {
    const float distance = horizontal_distance(m_position, player.position);
    const float speed = (m_phase == 1 ? 5.2f : (m_phase == 2 ? 7.8f : 10.6f)) * m_effects.speed_factor();
    glm::vec3 target = predict_player(player, 0.36f);

    // Учитываем привычный обход с одного фланга. Чем чаще игрок заходит за спину,
    // тем раньше рыцарь разворачивается и тем сильнее корректирует позицию.
    if (m_rear_habit > 0.35f && distance < 15.0f) {
        rotate_towards(target, dt, 1.7f + m_rear_habit);
        if (m_preferred_orbit != 0 && distance > 4.5f) {
            const glm::vec3 fwd = forward_from_yaw(m_yaw);
            const glm::vec3 right(fwd.z, 0.0f, -fwd.x);
            target += right * static_cast<float>(m_preferred_orbit) * 2.0f * m_rear_habit;
        }
    }

    if (m_grounded && m_jump_cooldown <= 0.0f && distance > (m_phase == 1 ? 16.0f : 12.0f)) {
        begin_jump(target, m_phase, false);
        return;
    }

    if (distance > (m_phase == 1 ? 5.0f : 6.3f)) {
        move_towards(target, speed, dt, chunks);
        if (m_grounded && horizontal_distance(m_position, target) > 1.0f) set_animation("Walk", true);
    } else {
        rotate_towards(target, dt, m_rear_habit > 0.4f ? 2.0f : 1.0f);
        set_animation("Idle", true);
    }
}

void Knight_Ai::update_ai(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks, Knight_Tick_Result& result) {
    const bool los = player.alive && has_line_of_sight(player, chunks);
    const float distance = glm::distance(m_position + glm::vec3(0, 1.4f, 0), player.position);
    const float detect_range = m_phase == 1 ? 48.0f : (m_phase == 2 ? 56.0f : 64.0f);
    const bool in_awareness = los && distance <= detect_range;
    const bool visible = can_see_player(player, chunks);

    if (visible) {
        m_has_target = true;
        m_lost_target_timer = 0.0f;
        m_last_seen_position = player.position;
    } else if (in_awareness) {
        // Периферийное обнаружение: рыцарь замечает движение вне фронтального
        // сектора и поворачивается к игроку. Can_See_Player всё равно остаётся
        // обязательным условием для начала атаки и каждого попадания.
        m_has_target = true;
        m_lost_target_timer = 0.0f;
        m_last_seen_position = player.position;
    } else if (m_has_target) {
        m_lost_target_timer += dt;
        if (m_lost_target_timer > 5.0f || distance > detect_range * 1.5f) {
            m_has_target = false;
            m_state = Knight_State::Returning;
        }
    }

    update_learning(player, dt);

    if (!m_has_target && m_state != Knight_State::Returning) {
        m_state = Knight_State::Dormant;
        set_animation("Idle", true);
        return;
    }

    // Игрок виден в прямой видимости, но вышел из фронтального сектора: рыцарь
    // поворачивается к нему (и учится контратаковать фланговые обходы), но не атакует.
    if (in_awareness && !visible && m_state != Knight_State::Windup && m_state != Knight_State::Attacking &&
        m_state != Knight_State::Jumping && m_state != Knight_State::Recovery) {
        const glm::vec3 predicted = predict_player(player, 0.25f);
        rotate_towards(predicted, dt, m_rear_habit > 0.35f ? 2.25f : 1.5f);
        m_state = Knight_State::Chase;
        set_animation("Idle", true);
        return;
    }

    switch (m_state) {
        case Knight_State::Dormant:
            if (visible) m_state = Knight_State::Chase;
            break;
        case Knight_State::Chase: {
            if (!visible) {
                if (m_lost_target_timer < 2.0f) {
                    rotate_towards(m_last_seen_position, dt);
                    move_towards(m_last_seen_position, 3.0f, dt, chunks);
                } else {
                    m_state = Knight_State::Returning;
                }
                break;
            }
            update_movement(dt, player, chunks);
            if (m_state == Knight_State::Jumping) break;
            if (facing_player(player)) {
                const Knight_Attack attack = choose_attack(player, chunks);
                if (attack != Knight_Attack::None) start_attack(attack, player);
            }
            break;
        }
        case Knight_State::Windup:
            update_windup(dt, player, chunks);
            break;
        case Knight_State::Attacking:
            update_attack(dt, player, chunks, result);
            break;
        case Knight_State::Jumping:
            update_jump(dt, player, chunks, result);
            break;
        case Knight_State::Recovery:
            update_recovery(dt, player, chunks);
            break;
        case Knight_State::Returning: {
            const glm::vec3 home = m_arena.center;
            if (horizontal_distance(m_position, home) > 3.0f) {
                move_towards(home, 4.0f, dt, chunks);
                set_animation("Walk", true);
            } else {
                m_state = Knight_State::Dormant;
                m_has_target = false;
                set_animation("Idle", true);
            }
            if (visible) m_state = Knight_State::Chase;
            break;
        }
        case Knight_State::Dead:
            break;
    }
}

Knight_Tick_Result Knight_Ai::update(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks) {
    Knight_Tick_Result result;
    dt = glm::clamp(dt, 0.0f, 0.05f);
    if (dt <= 0.0f) return result;
    m_clock += dt;
    const int regeneration = m_effects.update(dt);
    if (regeneration > 0) m_health = std::min(MAX_HEALTH, m_health + static_cast<float>(regeneration));
    m_attack_cooldown = std::max(0.0f, m_attack_cooldown - dt);
    m_jump_cooldown = std::max(0.0f, m_jump_cooldown - dt);
    m_hurt_timer = std::max(0.0f, m_hurt_timer - dt);
    m_model.set_hurt_flash(m_hurt_timer / kHurtSeconds);

    if (m_dying) {
        m_death_timer += dt;
        m_model.update(dt);
        return result;
    }

    update_phase();
    update_ai(dt, player, chunks, result);

    // Гравитация только при движении/прыжке. Если босс сошёл с уступа, он падает,
    // но никогда не пересекает физический пол арены.
    if (m_state != Knight_State::Jumping && !m_grounded) {
        m_velocity.y -= GRAVITY * dt;
        glm::vec3 next = m_position;
        next.y += m_velocity.y * dt;
        next = m_arena.clamp(next, 1.5f);
        if (can_occupy(next, chunks)) m_position.y = next.y;
        else if (m_velocity.y < 0.0f) m_grounded = true, m_velocity.y = 0.0f;
    }
    if (!m_grounded && has_ground_below(m_position, chunks) && m_velocity.y <= 0.0f) {
        m_grounded = true;
        m_velocity.y = 0.0f;
    }
    if (m_grounded) m_position.y = std::max(m_position.y, m_arena.floor_y);
    m_position = m_arena.clamp(m_position, 1.5f);
    m_model.set_position(m_position);
    m_model.set_yaw_degrees(glm::degrees(m_yaw));
    m_model.update(dt);
    return result;
}

bool Knight_Ai::take_damage(float amount, const glm::vec3& attacker_position) {
    if (m_dying || amount <= 0.0f) return false;
    m_health = std::max(0.0f, m_health - amount);
    m_hurt_timer = kHurtSeconds;
    if (!m_has_target) {
        m_has_target = true;
        m_last_seen_position = attacker_position;
    }
    m_current_attack = Knight_Attack::None;
    m_state = Knight_State::Recovery;
    m_state_timer = 0.28f;
    m_attack_cooldown = std::max(m_attack_cooldown, 0.25f);
    if (m_model.has_animation("Hit")) set_animation("Hit", false);
    if (m_health <= 0.0f) return kill();
    update_phase();
    return false;
}

bool Knight_Ai::kill() {
    if (m_dying) return false;
    m_dying = true;
    m_health = 0.0f;
    m_death_timer = 0.0f;
    m_state = Knight_State::Dead;
    set_animation("Death", false);
    return true;
}

void Knight_Ai::compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const {
    out_min = m_position + glm::vec3(-BODY_HALF_X, 0.05f, -BODY_HALF_Z);
    out_max = m_position + glm::vec3(BODY_HALF_X, BODY_HEIGHT, BODY_HALF_Z);
}

void Knight_Ai::render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient) const {
    m_model.render(shader, view, projection, ambient);
}
