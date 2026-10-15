#include "entities/animals/Passive_Animal.h"
#include "world/chunks/Chunk_Manager.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace {
    constexpr float k_hitbox_margin = 0.1f;
}

// Таблица видов. Все числа — из Config; добавить вид = добавить строку.
const Passive_Stats& passive_stats(Passive_Kind kind) {
        static const Passive_Stats cow{
            "Cow", "assets/mobs/cow.glb",
            Config::min_cow_count, Config::cow_spawn_radius_min, Config::cow_spawn_radius_max,
            Config::cow_walk_speed, Config::cow_width, Config::cow_height, Config::cow_max_health,
            Config::cow_panic_seconds, Config::cow_panic_speed, Config::cow_invulnerable_seconds,
            Config::cow_death_seconds, Config::cow_respawn_delay,
            Block_Types::Wheat, Block_Types::Raw_Beef, Config::cow_drop_min, Config::cow_drop_max,
            true,   // корова плавает
        };
        static const Passive_Stats sheep{
                "Sheep", "assets/mobs/sheep.glb",
                Config::min_sheep_count, Config::sheep_spawn_radius_min, Config::sheep_spawn_radius_max,
                Config::sheep_walk_speed, Config::sheep_width, Config::sheep_height, Config::sheep_max_health,
                Config::sheep_panic_seconds, Config::sheep_panic_speed, Config::sheep_invulnerable_seconds,
                Config::sheep_death_seconds, Config::sheep_respawn_delay,
                Block_Types::Wheat, Block_Types::Air, 0, 0, // шерсти и баранины пока нет в блоках — ничего не падает
                true,   // овца плавает
        };
    return kind == Passive_Kind::Sheep ? sheep : cow;
}

Passive_Animal::Passive_Animal(Passive_Kind kind, std::string name, const glm::vec3& spawn_position)
    : m_kind(kind), m_name(std::move(name)), m_rng(std::random_device{}()) {
    m_health = passive_stats(kind).max_health;
    m_mob.set_position(spawn_position);
    std::uniform_real_distribution<float> t(0.5f, 5.0f);
    m_state_timer = t(m_rng);
    std::uniform_real_distribution<float> a(0.0f, 6.2831853f);
    const float angle = a(m_rng);
    m_walk_direction = glm::vec3(std::cos(angle), 0.0f, std::sin(angle));
    m_mob.set_yaw_degrees(glm::degrees(std::atan2(m_walk_direction.x, m_walk_direction.z)));
}

bool Passive_Animal::load_model() {
    const bool ok = m_mob.load(stats().model_path);
    if (ok && m_mob.has_animation("Idle")) m_mob.play_animation("Idle");
    return ok;
}

void Passive_Animal::set_walking(bool walking) {
    m_walking = walking;
    if (walking) {
        std::uniform_real_distribution<float> angle_dist(0.0f, 6.2831853f);
        const float angle = angle_dist(m_rng);
        m_walk_direction = glm::vec3(std::cos(angle), 0.0f, std::sin(angle));
        m_mob.set_yaw_degrees(glm::degrees(std::atan2(m_walk_direction.x, m_walk_direction.z)));
    }
    const char* animation = walking ? "Walk" : "Idle";
    if (m_mob.has_animation(animation)) m_mob.play_animation(animation);
}

bool Passive_Animal::collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const {
    constexpr float epsilon = 0.001f;
    const Passive_Stats& s = stats();
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

void Passive_Animal::compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const {
    const Passive_Stats& s = stats();
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

glm::vec3 Passive_Animal::get_hitbox_min() const { glm::vec3 lo, hi; compute_hitbox(lo, hi); return lo; }
glm::vec3 Passive_Animal::get_hitbox_max() const { glm::vec3 lo, hi; compute_hitbox(lo, hi); return hi; }

bool Passive_Animal::take_drowning_damage(float amount) {
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

bool Passive_Animal::take_damage(float amount, const glm::vec3& attacker_position) {
    if (m_dying || amount <= 0.0f || m_invulnerable_timer > 0.0f) return false;
    m_health -= amount;
    m_invulnerable_timer = stats().invulnerable_seconds;
    m_hurt_flash_timer = 0.4f;

    glm::vec3 away = m_mob.get_position() - attacker_position;
    away.y = 0.0f;
    if (glm::length(away) < 0.001f) away = glm::vec3(1.0f, 0.0f, 0.0f);
    away = glm::normalize(away);
    m_knockback = away * 5.0f;
    m_vertical_velocity = 4.0f;
    m_on_ground = false;

    if (m_health <= 0.0f) {
        m_dying = true;
        m_death_timer = 0.0f;
        m_knockback = glm::vec3(0.0f);
        return true;
    }
    m_panic_timer = stats().panic_seconds;
    m_walk_direction = away;
    m_mob.set_yaw_degrees(glm::degrees(std::atan2(away.x, away.z)));
    if (m_mob.has_animation("Walk")) m_mob.play_animation("Walk");
    return false;
}

void Passive_Animal::seek_towards(const glm::vec3& target) {
    glm::vec3 d = target - m_mob.get_position();
    d.y = 0.0f;
    if (glm::length(d) < 0.05f) return;
    m_seek_dir = glm::normalize(d);
    if (m_seek_timer <= 0.0f && m_mob.has_animation("Walk")) m_mob.play_animation("Walk");
    m_seek_timer = 0.3f;
    m_mob.set_yaw_degrees(glm::degrees(std::atan2(m_seek_dir.x, m_seek_dir.z)));
}

void Passive_Animal::update(float dt, const Chunk_Manager& chunk_manager) {
    const Passive_Stats& s = stats();
    if (m_dying) {
        m_death_timer += dt;
        const float fall_t = std::clamp(m_death_timer / (s.death_seconds * 0.75f), 0.0f, 1.0f);
        m_mob.set_roll_pivot_height(s.width * 0.5f);
        m_mob.set_roll_degrees(90.0f * fall_t * fall_t);
        m_mob.set_hurt_flash(0.8f);
        m_mob.update(dt);
        return;
    }

    m_breeding.update(dt);
    m_mob.set_scale(m_breeding.size_factor());
    m_invulnerable_timer = std::max(0.0f, m_invulnerable_timer - dt);
    m_hurt_flash_timer = std::max(0.0f, m_hurt_flash_timer - dt);
    m_mob.set_hurt_flash(m_hurt_flash_timer / 0.4f);

    const bool was_panicking = m_panic_timer > 0.0f;
    m_panic_timer = std::max(0.0f, m_panic_timer - dt);
    const bool was_seeking = m_seek_timer > 0.0f;
    m_seek_timer = std::max(0.0f, m_seek_timer - dt);
    if ((was_panicking && m_panic_timer <= 0.0f) || (was_seeking && m_seek_timer <= 0.0f)) {
        const char* animation = m_walking ? "Walk" : "Idle";
        if (m_mob.has_animation(animation)) m_mob.play_animation(animation);
    }

    // Простое «мышление»: постоять — пройтись — постоять.
    m_state_timer -= dt;
    if (m_state_timer <= 0.0f) {
        std::uniform_real_distribution<float> idle_time(3.0f, 9.0f), walk_time(2.0f, 5.0f);
        set_walking(!m_walking);
        m_state_timer = m_walking ? walk_time(m_rng) : idle_time(m_rng);
    }

    glm::vec3 position = m_mob.get_position();
    // Вода: плавающие всплывают, остальные тонут; воздух тратится, пока голова под водой.
    const Liquid_Physics::Sample wet = Liquid_Physics::sample(chunk_manager, position, s.height);
    if (const int events = m_breath.tick(dt, wet.head_in_liquid, Config::breath_max_seconds)) {
        take_drowning_damage(static_cast<float>(events) * Config::drown_damage);
    }
    if (wet.body_in_liquid) {
        m_vertical_velocity = Liquid_Physics::swim_vertical_velocity(wet, s.can_swim, false, m_vertical_velocity, dt);
    } else {
        m_vertical_velocity = std::max(Config::mob_max_fall_speed, m_vertical_velocity + Config::mob_gravity * dt);
    }

    const bool panicking = m_panic_timer > 0.0f;
    const bool seeking = m_seek_timer > 0.0f && !panicking;
    glm::vec3 velocity = m_knockback;
    if (panicking) velocity += m_walk_direction * s.panic_speed;
    else if (seeking) velocity += m_seek_dir * s.walk_speed * 1.8f;
    else if (m_walking) velocity += m_walk_direction * s.walk_speed;
    m_knockback *= std::exp(-6.0f * dt);
    if (glm::length(m_knockback) < 0.05f) m_knockback = glm::vec3(0.0f);

    // В воде замедляется и сносится течением.
    velocity = velocity * Liquid_Physics::horizontal_drag(wet) + wet.flow;

    bool blocked = false;
    glm::vec3 target = position;
    target.x += velocity.x * dt;
    if (!collides_at(target, chunk_manager)) position.x = target.x;
    else if (std::abs(velocity.x) > 0.01f) blocked = true;
    target = position;
    target.z += velocity.z * dt;
    if (!collides_at(target, chunk_manager)) position.z = target.z;
    else if (std::abs(velocity.z) > 0.01f) blocked = true;

    if (blocked && m_on_ground) {
        if (panicking || seeking || m_walking) { m_vertical_velocity = 7.0f; m_on_ground = false; }
        if (m_walking && !panicking && !seeking) { m_state_timer = 0.0f; } // упёрлась — выбирает новое направление
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
}

void Passive_Animal::render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const {
    m_mob.render(shader, view, projection, ambient_intensity);
}
