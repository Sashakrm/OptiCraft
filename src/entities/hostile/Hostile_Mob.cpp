//
// Created by noktemor on 15.10.2026.
//
#include "entities/hostile/Hostile_Mob.h"
#include "world/chunks/Chunk_Manager.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace {
    constexpr float k_hitbox_margin = 0.1f;
}

const Hostile_Stats& hostile_stats(Hostile_Kind kind) {
    static const Hostile_Stats zombie{
        "Zombie", "assets/mobs/zombie.glb",
        Config::min_zombie_count, Config::zombie_spawn_radius_min, Config::zombie_spawn_radius_max,
        Config::zombie_walk_speed, Config::zombie_run_speed, Config::zombie_detect_range,
        Config::zombie_width, Config::zombie_height, Config::zombie_max_health,
        Config::zombie_attack_damage, Config::zombie_attack_reach, Config::zombie_attack_cooldown,
        Config::zombie_invulnerable_seconds, Config::zombie_death_seconds,
        false,   // зомби не плавает — тонет
    };
    static const Hostile_Stats skeleton{
        "Skeleton", "assets/mobs/skeleton.glb",
        Config::min_skeleton_count, Config::skeleton_spawn_radius_min, Config::skeleton_spawn_radius_max,
        Config::skeleton_walk_speed, Config::skeleton_run_speed, Config::skeleton_detect_range,
        Config::skeleton_width, Config::skeleton_height, Config::skeleton_max_health,
        Config::skeleton_attack_damage, Config::skeleton_attack_reach, Config::skeleton_attack_cooldown,
        Config::skeleton_invulnerable_seconds, Config::skeleton_death_seconds,
        false,   // скелет не плавает — тонет
    };
    return kind == Hostile_Kind::Skeleton ? skeleton : zombie;
}

Hostile_Mob::Hostile_Mob(Hostile_Kind kind, std::string name, const glm::vec3& spawn_position)
    : m_kind(kind), m_name(std::move(name)), m_rng(std::random_device{}()) {
    m_health = hostile_stats(kind).max_health;
    m_mob.set_position(spawn_position);
    std::uniform_real_distribution<float> t(0.5f, 4.0f);
    m_state_timer = t(m_rng);
    std::uniform_real_distribution<float> a(0.0f, 6.2831853f);
    const float angle = a(m_rng);
    m_walk_direction = glm::vec3(std::cos(angle), 0.0f, std::sin(angle));
}

bool Hostile_Mob::load_model() {
    const bool ok = m_mob.load(stats().model_path);
    if (ok && m_mob.has_animation("Idle")) m_mob.play_animation("Idle");
    return ok;
}

void Hostile_Mob::play_clip(const char* name, bool loop) {
    if (m_mob.has_animation(name)) m_mob.play_animation(name, loop);
}

void Hostile_Mob::set_walking(bool walking) {
    m_walking = walking;
    if (walking) {
        std::uniform_real_distribution<float> angle_dist(0.0f, 6.2831853f);
        const float angle = angle_dist(m_rng);
        m_walk_direction = glm::vec3(std::cos(angle), 0.0f, std::sin(angle));
        m_mob.set_yaw_degrees(glm::degrees(std::atan2(m_walk_direction.x, m_walk_direction.z)));
    }
    play_clip(walking ? "Walk" : "Idle");
}

void Hostile_Mob::alert(const glm::vec3& player_position) {
    if (m_dying) return;
    m_memory_timer = Config::hostile_memory_seconds;
    m_target_position = player_position;
}

bool Hostile_Mob::collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const {
    constexpr float epsilon = 0.001f;
    const Hostile_Stats& s = stats();
    const float half_width = s.width * 0.5f;
    const int min_x = static_cast<int>(std::floor(position.x - half_width + epsilon));
    const int max_x = static_cast<int>(std::floor(position.x + half_width - epsilon));
    const int min_y = static_cast<int>(std::floor(position.y + epsilon));
    const int max_y = static_cast<int>(std::floor(position.y + s.height - epsilon));
    const int min_z = static_cast<int>(std::floor(position.z - half_width + epsilon));
    const int max_z = static_cast<int>(std::floor(position.z + half_width - epsilon));
    for (int y = min_y; y <= max_y; ++y)
        for (int z = min_z; z <= max_z; ++z)
            for (int x = min_x; x <= max_x; ++x)
                if (get_block_props(chunk_manager.get_block_world(x, y, z)).is_solid) return true;
    return false;
}

void Hostile_Mob::compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const {
    const Hostile_Stats& s = stats();
    const glm::vec3& p = m_mob.get_position();
    const float half = s.width * 0.5f;
    out_min = glm::vec3(p.x - half, p.y, p.z - half);
    out_max = glm::vec3(p.x + half, p.y + s.height, p.z + half);
    glm::vec3 lo, hi;
    if (!m_mob.get_local_bounds(lo, hi)) {
        out_min -= glm::vec3(k_hitbox_margin);
        out_max += glm::vec3(k_hitbox_margin);
        return;
    }
    const float scale = m_mob.get_scale();
    const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(m_mob.get_yaw_degrees()), glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec3 wmin(1e30f), wmax(-1e30f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        const glm::vec3 w = glm::vec3(rot * glm::vec4(corner * scale, 1.0f)) + p;
        wmin = glm::min(wmin, w);
        wmax = glm::max(wmax, w);
    }
    out_min = wmin - glm::vec3(k_hitbox_margin);
    out_max = wmax + glm::vec3(k_hitbox_margin);
}

glm::vec3 Hostile_Mob::get_hitbox_min() const { glm::vec3 lo, hi; compute_hitbox(lo, hi); return lo; }
glm::vec3 Hostile_Mob::get_hitbox_max() const { glm::vec3 lo, hi; compute_hitbox(lo, hi); return hi; }

bool Hostile_Mob::take_drowning_damage(float amount) {
    if (m_dying || amount <= 0.0f) return false;
    m_health -= amount;
    m_hurt_flash_timer = 0.4f;
    if (m_health <= 0.0f) {
        m_dying = true;
        m_death_timer = 0.0f;
        m_knockback = glm::vec3(0.0f);
        return true;
    }
    return false;
}

bool Hostile_Mob::take_damage(float amount, const glm::vec3& attacker_position) {
    if (m_dying || amount <= 0.0f || m_invulnerable_timer > 0.0f) return false;
    m_health -= amount;
    m_invulnerable_timer = stats().invulnerable_seconds;
    m_hurt_flash_timer = 0.4f;

    glm::vec3 away = m_mob.get_position() - attacker_position;
    away.y = 0.0f;
    if (glm::length(away) < 0.001f) away = glm::vec3(1.0f, 0.0f, 0.0f);
    away = glm::normalize(away);
    m_knockback = away * 3.0f;
    m_vertical_velocity = 4.0f;
    m_on_ground = false;

    if (m_health <= 0.0f) {
        m_dying = true;
        m_death_timer = 0.0f;
        m_knockback = glm::vec3(0.0f);
        return true;
    }
    // Ударили — враг злится и бежит за обидчиком.
    alert(attacker_position);
    return false;
}

int Hostile_Mob::update(float dt, const Chunk_Manager& chunk_manager, const Hostile_Context& context) {
    const Hostile_Stats& s = stats();
    if (m_dying) {
        m_death_timer += dt;
        const float fall_t = std::clamp(m_death_timer / (s.death_seconds * 0.75f), 0.0f, 1.0f);
        m_mob.set_roll_pivot_height(s.width * 0.5f);
        m_mob.set_roll_degrees(90.0f * fall_t * fall_t);
        m_mob.set_hurt_flash(0.8f);
        m_mob.update(dt);
        return 0;
    }

    m_invulnerable_timer = std::max(0.0f, m_invulnerable_timer - dt);
    m_hurt_flash_timer = std::max(0.0f, m_hurt_flash_timer - dt);
    m_mob.set_hurt_flash(m_hurt_flash_timer / 0.4f);
    m_attack_cooldown = std::max(0.0f, m_attack_cooldown - dt);

    // --- Память о преследовании --------------------------------------------------------
    if (!context.player_alive) {
        m_memory_timer = 0.0f;
    } else if (context.player_seen) {
        m_memory_timer = Config::hostile_memory_seconds;
        m_target_position = context.player_position;
    } else {
        m_memory_timer = std::max(0.0f, m_memory_timer - dt);
    }

    glm::vec3 position = m_mob.get_position();
    // Вода: зомби и скелет тонут (can_swim = false), воздух тратится, пока голова под водой.
    const Liquid_Physics::Sample wet = Liquid_Physics::sample(chunk_manager, position, s.height);
    if (const int events = m_breath.tick(dt, wet.head_in_liquid, Config::breath_max_seconds)) {
        take_drowning_damage(static_cast<float>(events) * Config::drown_damage);
    }
    if (wet.body_in_liquid) {
        m_vertical_velocity = Liquid_Physics::swim_vertical_velocity(wet, s.can_swim, false, m_vertical_velocity, dt);
    } else {
        m_vertical_velocity = std::max(Config::mob_max_fall_speed, m_vertical_velocity + Config::mob_gravity * dt);
    }
    m_knockback *= std::exp(-6.0f * dt);
    if (glm::length(m_knockback) < 0.05f) m_knockback = glm::vec3(0.0f);

    glm::vec3 horizontal{0.0f};   // желаемая скорость по XZ
    int damage = 0;

    if (m_memory_timer > 0.0f) {
        // --- Погоня -----------------------------------------------------------------------
        glm::vec3 to_target = m_target_position - position;
        to_target.y = 0.0f;
        const float dist = glm::length(to_target);
        const glm::vec3 dir = dist > 0.001f ? to_target / dist : glm::vec3(0.0f);
        if (dir != glm::vec3(0.0f)) {
            m_mob.set_yaw_degrees(glm::degrees(std::atan2(dir.x, dir.z)));
        }

        const bool vertical_ok = std::abs(m_target_position.y - position.y) <= Config::hostile_attack_height_tolerance;
        if (dist <= s.attack_reach && vertical_ok) {
            // В упор: стоим и бьём по кулдауну.
            if (m_attack_cooldown <= 0.0f && context.player_alive) {
                m_attack_cooldown = s.attack_cooldown;
                damage = static_cast<int>(std::ceil(s.attack_damage));
                play_clip("Attack", false);
            } else if (!(m_mob.has_animation("Attack") && !m_mob.is_animation_finished())) {
                // Замах доигран (или клипа Attack нет) — стоим на месте.
                play_clip("Idle");
            }
        } else {
            // Бежим к игроку (или к последней точке, где его видели).
            if (!context.player_seen && dist < 0.8f) {
                // Пришли к последней известной точке, а игрока нет — теряем след.
                m_memory_timer = 0.0f;
            } else {
                // Обход: если недавно не сдвинулись, на секунду уходим боком от прямой линии.
                m_stuck_check_timer += dt;
                if (m_stuck_check_timer >= Config::hostile_stuck_check_seconds) {
                    const float moved = glm::length(position - m_stuck_check_position);
                    if (moved < 0.2f && m_detour_timer <= 0.0f) {
                        m_detour_timer = Config::hostile_detour_seconds;
                        const float side = std::uniform_int_distribution<int>(0, 1)(m_rng) == 0 ? 1.0f : -1.0f;
                        m_detour_direction = glm::normalize(glm::vec3(-dir.z, 0.0f, dir.x)) * side;
                    }
                    m_stuck_check_position = position;
                    m_stuck_check_timer = 0.0f;
                }
                if (m_detour_timer > 0.0f) {
                    m_detour_timer = std::max(0.0f, m_detour_timer - dt);
                    horizontal = m_detour_direction * s.run_speed;
                } else {
                    horizontal = dir * s.run_speed;
                }
                play_clip("Walk");
            }
        }
    } else {
        // --- Блуждание -------------------------------------------------------------------
        m_state_timer -= dt;
        if (m_state_timer <= 0.0f) {
            std::uniform_real_distribution<float> idle_time(3.0f, 8.0f), walk_time(2.0f, 4.0f);
            set_walking(!m_walking);
            m_state_timer = m_walking ? walk_time(m_rng) : idle_time(m_rng);
        }
        if (m_walking) horizontal = m_walk_direction * s.walk_speed;
    }

    horizontal += m_knockback;
    // В воде замедляется и сносится течением.
    horizontal = horizontal * Liquid_Physics::horizontal_drag(wet) + wet.flow;

    // --- Физика: как у пассивных животных, с перепрыгиванием уступа при погоне -----------
    bool blocked = false;
    glm::vec3 target = position;
    target.x += horizontal.x * dt;
    if (!collides_at(target, chunk_manager)) position.x = target.x;
    else if (std::abs(horizontal.x) > 0.01f) blocked = true;
    target = position;
    target.z += horizontal.z * dt;
    if (!collides_at(target, chunk_manager)) position.z = target.z;
    else if (std::abs(horizontal.z) > 0.01f) blocked = true;

    if (blocked && m_on_ground && m_memory_timer > 0.0f) {
        m_vertical_velocity = 7.0f;
        m_on_ground = false;
    }

    target = position;
    target.y += m_vertical_velocity * dt;
    if (collides_at(target, chunk_manager)) {
        if (m_vertical_velocity < 0.0f) m_on_ground = true;
        m_vertical_velocity = 0.0f;
    } else {
        position.y = target.y;
        m_on_ground = false;
    }

    m_mob.set_position(position);
    m_mob.update(dt);
    return damage;
}

void Hostile_Mob::render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const {
    m_mob.render(shader, view, projection, ambient_intensity);
}
