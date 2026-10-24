#include "entities/animals/Pig.h"
#include "utils/Config.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Types.h"
#include <cmath>
#include <algorithm>
#include <glm/trigonometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

Pig::Pig(std::string name, const glm::vec3& spawn_position)
    : m_ai(std::move(name))
    , m_rng(std::random_device{}()) {
    m_mob.set_position(spawn_position);
}

bool Pig::load_model(const std::string& glb_path) {
    return m_mob.load(glb_path);
}

void Pig::pick_new_walk_direction() {
    std::uniform_real_distribution<float> angle_dist(0.0f, 6.2831853f); // 2*pi
    const float angle = angle_dist(m_rng);
    m_walk_direction = glm::vec3(std::cos(angle), 0.0f, std::sin(angle));
    // Ориентация модели "лицом по курсу" — приблизительно: зависит от того,
    // в какую сторону смотрит модель в Blender (+Z/-Z), поэтому чисто
    // косметическая деталь, а не что-то, на что стоит полагаться геймплейно.
    m_mob.set_yaw_degrees(glm::degrees(std::atan2(m_walk_direction.x, m_walk_direction.z)));
}

const char* Pig::animation_name_for_state(Pig_State state) {
    switch (state) {
        case Pig_State::Eating:   return "Eat";
        case Pig_State::Sleeping: return "Sleep";
        case Pig_State::Walking:  return "Walk";
        case Pig_State::Happy:    return "Happy";
        default:                  return "Idle"; // Idle/Hungry/Sleepy — всё ещё "просто стоит"
    }
}

bool Pig::collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const {
    constexpr float epsilon = 0.001f;
    const float half_width = Config::pig_width * 0.5f;

    const int min_x = static_cast<int>(std::floor(position.x - half_width + epsilon));
    const int max_x = static_cast<int>(std::floor(position.x + half_width - epsilon));
    const int min_y = static_cast<int>(std::floor(position.y + epsilon));
    const int max_y = static_cast<int>(std::floor(position.y + Config::pig_height - epsilon));
    const int min_z = static_cast<int>(std::floor(position.z - half_width + epsilon));
    const int max_z = static_cast<int>(std::floor(position.z + half_width - epsilon));

    for (int y = min_y; y <= max_y; ++y) {
        for (int z = min_z; z <= max_z; ++z) {
            for (int x = min_x; x <= max_x; ++x) {
                const Block_Types type = chunk_manager.get_block_world(x, y, z);
                if (get_block_props(type).is_solid) {
                    return true;
                }
            }
        }
    }
    return false;
}

namespace {
    // Запас вокруг хитбокса, блоков: как в Minecraft, луч по мобу чуть прощает (там 0.1).
    constexpr float k_hitbox_margin = 0.1f;
}

// Хитбокс для ПОПАДАНИЯ ЛУЧОМ строится по реальной геометрии модели (то, что игрок видит и в
// что целится), повёрнутой по yaw свиньи. Коллизия с миром — отдельно и берётся из Config
// (pig_width/pig_height): модель может быть длиннее, чем она широка, а квадратный AABB 0.6x0.6
// не покрывал бы ни голову, ни зад — из-за этого часть ударов «проходила сквозь свинью».
void Pig::compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const {
    const glm::vec3& p = m_mob.get_position();
    const float half = Config::pig_width * 0.5f;
    // Запасной вариант, если модель не загрузилась: габариты коллизии.
    out_min = glm::vec3(p.x - half, p.y, p.z - half);
    out_max = glm::vec3(p.x + half, p.y + Config::pig_height, p.z + half);

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
    // Только то, что реально нарисовано (+ запас): иначе по воздуху под «парящим» телом модели
    // (тело pig.glb висит на ~1 блок выше ног) можно было бы бить.
    out_min = wmin - glm::vec3(k_hitbox_margin);
    out_max = wmax + glm::vec3(k_hitbox_margin);
}

glm::vec3 Pig::get_hitbox_min() const {
    glm::vec3 lo, hi;
    compute_hitbox(lo, hi);
    return lo;
}

glm::vec3 Pig::get_hitbox_max() const {
    glm::vec3 lo, hi;
    compute_hitbox(lo, hi);
    return hi;
}

bool Pig::take_drowning_damage(float amount) {
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

bool Pig::take_damage(float amount, const glm::vec3& attacker_position) {
    if (m_dying || amount <= 0.0f || m_invulnerable_timer > 0.0f) return false;

    m_health -= amount;
    m_invulnerable_timer = Config::pig_invulnerable_seconds;
    m_hurt_flash_timer = 0.4f;

    // Направление ОТ игрока по горизонтали: и отбрасывание, и бегство идут туда.
    glm::vec3 away = m_mob.get_position() - attacker_position;
    away.y = 0.0f;
    if (glm::length(away) < 0.001f) away = glm::vec3(1.0f, 0.0f, 0.0f);
    away = glm::normalize(away);

    m_knockback = away * 5.0f;
    m_vertical_velocity = 4.0f; // небольшой подскок при ударе
    m_on_ground = false;

    if (m_health <= 0.0f) {
        m_dying = true;
        m_death_timer = 0.0f;
        m_knockback = glm::vec3(0.0f);
        return true;
    }

    m_panic_timer = Config::pig_panic_seconds;
    m_walk_direction = away;
    m_mob.set_yaw_degrees(glm::degrees(std::atan2(away.x, away.z)));
    if (m_mob.has_animation("Walk")) m_mob.play_animation("Walk");
    return false;
}

void Pig::seek_towards(const glm::vec3& target) {
    glm::vec3 d = target - m_mob.get_position();
    d.y = 0.0f;
    if (glm::length(d) < 0.05f) return;
    m_seek_dir = glm::normalize(d);
    if (m_seek_timer <= 0.0f && m_mob.has_animation("Walk")) m_mob.play_animation("Walk");
    m_seek_timer = 0.3f;
    m_mob.set_yaw_degrees(glm::degrees(std::atan2(m_seek_dir.x, m_seek_dir.z)));
}

void Pig::update(float delta_time, const Chunk_Manager& chunk_manager) {
    const int regen_ticks = m_effects.update(delta_time);
    if (!m_dying && regen_ticks > 0) m_health = std::min(Config::pig_max_health, m_health + static_cast<float>(regen_ticks));
    m_breeding.update(delta_time);
    m_mob.set_scale(m_breeding.size_factor());
    const bool was_seeking = m_seek_timer > 0.0f;
    m_seek_timer = std::max(0.0f, m_seek_timer - delta_time);
    if (was_seeking && m_seek_timer <= 0.0f && !m_dying) {
        const char* animation = animation_name_for_state(m_ai.get_state());
        if (m_mob.has_animation(animation)) m_mob.play_animation(animation);
    }

    if (m_dying) {
        // Падение на бок за ~75% анимации, остальное лежит; не двигается и не думает.
        m_death_timer += delta_time;
        const float fall_t = std::clamp(m_death_timer / (Config::pig_death_seconds * 0.75f), 0.0f, 1.0f);
        m_mob.set_roll_pivot_height(Config::pig_width * 0.5f);
        m_mob.set_roll_degrees(90.0f * fall_t * fall_t);
        m_mob.set_hurt_flash(0.8f);
        m_mob.update(delta_time);
        return;
    }

    m_invulnerable_timer = std::max(0.0f, m_invulnerable_timer - delta_time);
    m_hurt_flash_timer = std::max(0.0f, m_hurt_flash_timer - delta_time);
    const bool was_panicking = m_panic_timer > 0.0f;
    m_panic_timer = std::max(0.0f, m_panic_timer - delta_time);
    if (was_panicking && m_panic_timer <= 0.0f) {
        // Паника кончилась: возвращаем анимацию, соответствующую текущему состоянию ИИ (иначе
        // свинья осталась бы стоять с бегущими ногами до следующей смены состояния).
        const char* animation = animation_name_for_state(m_ai.get_state());
        if (m_mob.has_animation(animation)) m_mob.play_animation(animation);
    }
    m_mob.set_hurt_flash(m_hurt_flash_timer / 0.4f);

    m_ai.update(delta_time);

    const Pig_State state = m_ai.get_state();
    if (state != m_last_applied_state) {
        m_last_applied_state = state;
        if (state == Pig_State::Walking) pick_new_walk_direction();

        // Испуганная свинья бежит, что бы там ни решил ИИ, — анимация ходьбы не сбивается.
        const char* animation = m_panic_timer > 0.0f ? "Walk" : animation_name_for_state(state);
        if (m_mob.has_animation(animation)) m_mob.play_animation(animation);
    }

    glm::vec3 position = m_mob.get_position();

    // Вода: свинья плавает (всплывает), воздух тратится, пока голова под водой.
    const Liquid_Physics::Sample wet = Liquid_Physics::sample(chunk_manager, position, Config::pig_height);
    if (const int events = m_breath.tick(delta_time, wet.head_in_liquid, Config::breath_max_seconds)) {
        take_drowning_damage(static_cast<float>(events) * Config::drown_damage);
    }

    // Гравитация — та же схема, что и у игрока (Player::update): скорость копится
    // каждый кадр, ось падения проверяется отдельно от горизонтального движения,
    // чтобы свинья аккуратно останавливалась на первом блоке под ногами, а не
    // проваливалась сквозь пол/потолок при диагональном движении.
    if (wet.body_in_liquid) {
        m_vertical_velocity = Liquid_Physics::swim_vertical_velocity(wet, true, false, m_vertical_velocity, delta_time);
    } else {
        m_vertical_velocity = std::max(Config::mob_max_fall_speed,
                                        m_vertical_velocity + Config::mob_gravity * delta_time);
    }

    // Горизонтальное движение: испуганная свинья бежит быстрее и не останавливается, обычная —
    // только в состоянии Walking; поверх всего накладывается затухающее отбрасывание.
    const bool panicking = m_panic_timer > 0.0f;
    glm::vec3 horizontal_velocity = m_knockback;
    const bool seeking = m_seek_timer > 0.0f && !panicking;
    if (panicking) {
        horizontal_velocity += m_walk_direction * Config::pig_panic_speed;
    } else if (seeking) {
        horizontal_velocity += m_seek_dir * Config::pig_walk_speed * 1.8f;
    } else if (state == Pig_State::Walking) {
        horizontal_velocity += m_walk_direction * Config::pig_walk_speed;
    }
    m_knockback *= std::exp(-6.0f * delta_time);
    if (glm::length(m_knockback) < 0.05f) m_knockback = glm::vec3(0.0f);
    // В воде замедляется и сносится течением.
    horizontal_velocity = horizontal_velocity * Liquid_Physics::horizontal_drag(wet) * m_effects.speed_factor() + wet.flow;

    bool blocked = false;
    glm::vec3 target = position;
    target.x += horizontal_velocity.x * delta_time;
    if (!collides_at(target, chunk_manager)) {
        position.x = target.x;
    } else if (std::abs(horizontal_velocity.x) > 0.01f) {
        blocked = true;
    }

    target = position;
    target.z += horizontal_velocity.z * delta_time;
    if (!collides_at(target, chunk_manager)) {
        position.z = target.z;
    } else if (std::abs(horizontal_velocity.z) > 0.01f) {
        blocked = true;
    }

    // Убегая, свинья перепрыгивает уступ в один блок, а не упирается в него.
    if ((panicking || seeking) && blocked && m_on_ground) {
        m_vertical_velocity = 7.0f;
        m_on_ground = false;
    }

    target = position;
    target.y += m_vertical_velocity * delta_time;
    if (collides_at(target, chunk_manager)) {
        if (m_vertical_velocity < 0.0f) {
            m_on_ground = true;
        }
        m_vertical_velocity = 0.0f;
    } else {
        position.y = target.y;
        m_on_ground = false;
    }

    m_mob.set_position(position);
    m_mob.update(delta_time);
}

void Pig::render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const {
    m_mob.render(shader, view, projection, ambient_intensity);
}
