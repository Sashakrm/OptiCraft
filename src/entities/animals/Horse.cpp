#include "entities/animals/Horse.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Types.h"
#include <algorithm>
#include <cmath>
#include <glm/trigonometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace {
    constexpr float k_pi = 3.14159265f;
    constexpr float k_hitbox_margin = 0.1f;

    float wrap_pi(float a) {
        while (a > k_pi) a -= 2.0f * k_pi;
        while (a < -k_pi) a += 2.0f * k_pi;
        return a;
    }

    // Шанс, что лошадь «сдастся» при очередном рывке: растёт с каждой неудачной попыткой.
    float taming_success_chance(int attempts) {
        return std::min(0.15f + static_cast<float>(attempts) * 0.10f, 0.95f);
    }

    glm::vec3 forward_of(float yaw) { return glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)); }
}

Horse::Horse(std::string name, const glm::vec3& spawn_position)
    : m_ai(std::move(name))
    , m_rng(std::random_device{}()) {
    m_mob.set_position(spawn_position);
    m_yaw = rand_range(-k_pi, k_pi);
    m_mob.set_yaw_degrees(glm::degrees(m_yaw));
}

bool Horse::load_model(const std::string& glb_path) {
    return m_mob.load(glb_path);
}

void Horse::seek_towards(const glm::vec3& target) {
    m_seek_target = target;
    m_seek_timer = 0.3f;
}

bool Horse::feed(Block_Types food) {
    if (!accepts_food(food) || m_dying) return false;
    if (!m_breeding.can_be_fed()) return false;
    m_last_food = food;
    m_breeding.start_love();
    m_ai.enter_state(Horse_State::Happy);
    return true;
}

float Horse::rand01() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng); }
float Horse::rand_range(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(m_rng); }

const char* Horse::animation_name_for_state(Horse_State state) {
    switch (state) {
        case Horse_State::Wander:
        case Horse_State::Flee:
        case Horse_State::Scared: return "Walk";
        case Horse_State::Graze:  return "Eat";
        case Horse_State::Sleep:  return "Sleep";
        case Horse_State::Happy:  return "Happy";
        default:                  return "Idle";
    }
}

bool Horse::collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const {
    constexpr float epsilon = 0.001f;
    const float half_width = Config::horse_width * 0.5f;

    const int min_x = static_cast<int>(std::floor(position.x - half_width + epsilon));
    const int max_x = static_cast<int>(std::floor(position.x + half_width - epsilon));
    const int min_y = static_cast<int>(std::floor(position.y + epsilon));
    const int max_y = static_cast<int>(std::floor(position.y + Config::horse_height - epsilon));
    const int min_z = static_cast<int>(std::floor(position.z - half_width + epsilon));
    const int max_z = static_cast<int>(std::floor(position.z + half_width - epsilon));

    for (int y = min_y; y <= max_y; ++y) {
        for (int z = min_z; z <= max_z; ++z) {
            for (int x = min_x; x <= max_x; ++x) {
                if (get_block_props(chunk_manager.get_block_world(x, y, z)).is_solid) return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------
//  Хитбокс / урон
// ---------------------------------------------------------------------------------------
void Horse::compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const {
    const glm::vec3& p = m_mob.get_position();
    const float half = Config::horse_width * 0.5f;
    out_min = glm::vec3(p.x - half, p.y, p.z - half);
    out_max = glm::vec3(p.x + half, p.y + Config::horse_height, p.z + half);

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

glm::vec3 Horse::get_hitbox_min() const { glm::vec3 lo, hi; compute_hitbox(lo, hi); return lo; }
glm::vec3 Horse::get_hitbox_max() const { glm::vec3 lo, hi; compute_hitbox(lo, hi); return hi; }

bool Horse::take_drowning_damage(float amount) {
    if (m_dying || amount <= 0.0f) return false;
    m_health -= amount;
    m_hurt_flash_timer = 0.4f;
    if (m_health <= 0.0f) {
        kill();
        return true;
    }
    return false;
}

bool Horse::take_damage(float amount, const glm::vec3& attacker_position) {
    if (m_dying || amount <= 0.0f || m_invulnerable_timer > 0.0f) return false;

    m_health -= amount;
    m_invulnerable_timer = Config::horse_invulnerable_seconds;
    m_hurt_flash_timer = 0.4f;

    glm::vec3 away = m_mob.get_position() - attacker_position;
    away.y = 0.0f;
    if (glm::length(away) < 0.001f) away = glm::vec3(1.0f, 0.0f, 0.0f);
    away = glm::normalize(away);

    m_knockback = away * 5.0f;
    m_vertical_velocity = 4.0f;
    m_on_ground = false;

    if (m_health <= 0.0f) {
        kill();
        return true;
    }

    // OnHorseDamaged из гайда: страх на максимум и паническое бегство от обидчика.
    m_ai.set_fear(100.0f);
    m_ai.add_fear(0.0f, attacker_position);
    if (!m_has_rider) m_ai.enter_state(Horse_State::Flee);
    return false;
}

// ---------------------------------------------------------------------------------------
//  Нападение
// ---------------------------------------------------------------------------------------
float Horse::compute_attack_damage() const {
    if (m_breeding.is_baby()) return 0.0f;
    const float damage = Config::horse_attack_damage + get_speed() * Config::horse_attack_speed_bonus;
    return std::min(damage, Config::horse_attack_max_damage);
}

bool Horse::can_attack() const {
    return !m_dying && !m_breeding.is_baby() && m_attack_cooldown <= 0.0f;
}

Horse_Attack_Result Horse::attack(const glm::vec3& target_min, const glm::vec3& target_max) {
    Horse_Attack_Result result;
    if (!can_attack()) return result;

    const glm::vec3 p = m_mob.get_position();
    const glm::vec3 closest = glm::clamp(p + glm::vec3(0.0f, Config::horse_height * 0.5f, 0.0f),
                                         target_min, target_max);
    const glm::vec3 centre = p + glm::vec3(0.0f, Config::horse_height * 0.5f, 0.0f);
    if (glm::length(closest - centre) > Config::horse_attack_reach) return result;

    glm::vec3 dir = 0.5f * (target_min + target_max) - p;
    dir.y = 0.0f;
    if (glm::length(dir) < 0.001f) dir = forward_of(m_yaw);
    dir = glm::normalize(dir);

    result.hit = true;
    result.damage = compute_attack_damage();
    result.knockback = dir * Config::horse_attack_knockback + glm::vec3(0.0f, 4.0f, 0.0f);

    m_attack_cooldown = Config::horse_attack_cooldown;
    m_yaw = std::atan2(dir.x, dir.z); // морда к цели
    return result;
}

Horse_Attack_Result Horse::attack(const glm::vec3& target_position) {
    const glm::vec3 half(0.3f, 0.9f, 0.3f); // примерно габариты игрока вокруг точки
    return attack(target_position - half, target_position + half);
}

// ---------------------------------------------------------------------------------------
//  Посадка / высадка
// ---------------------------------------------------------------------------------------
bool Horse::mount() {
    if (m_dying || m_has_rider) return false;

    m_has_rider = true;
    m_sprinting = false;
    m_throw_request = false;
    m_taming_time = 0.0f;
    m_ride_velocity = glm::vec3(0.0f);
    m_knockback = glm::vec3(0.0f);
    m_ai.set_fear(0.0f);
    m_ai.enter_state(m_tamed ? Horse_State::Ridden : Horse_State::Taming);
    return true;
}

void Horse::dismount(const glm::vec3& rider_position) {
    if (!m_has_rider) return;
    m_has_rider = false;
    m_seat_shake = glm::vec3(0.0f);
    m_drifting = false;
    m_sprinting = false;
    m_ride_velocity *= 0.3f; // пусть немного докатится

    if (m_tamed) {
        m_ai.enter_state(Horse_State::Idle);
    } else {
        // Дикая, сбросившая всадника: испугана и убегает от него.
        m_ai.add_fear(60.0f, rider_position);
        m_ai.enter_state(Horse_State::Scared);
    }
}

glm::vec3 Horse::get_seat_position() const {
    return m_mob.get_position() + glm::vec3(0.0f, Config::horse_seat_height, 0.0f) + m_seat_shake;
}

void Horse::finish_taming() {
    m_tamed = true;
    m_taming_progress = 1.0f;
    m_seat_shake = glm::vec3(0.0f);
    m_ride_velocity *= 0.3f;
    m_ai.set_fear(0.0f);
    m_ai.enter_state(Horse_State::Ridden);
}

void Horse::turn_towards(float target_yaw, float max_rate, float dt) {
    const float d = wrap_pi(target_yaw - m_yaw);
    const float step = std::clamp(d, -max_rate * dt, max_rate * dt);
    m_yaw += step;
    m_yaw_rate = dt > 0.0f ? step / dt : 0.0f;
}

// ---------------------------------------------------------------------------------------
//  Восприятие игрока (UpdateHorsePerception из гайда)
// ---------------------------------------------------------------------------------------
void Horse::update_perception(float dt, const Horse_Context& context) {
    if (m_tamed || !context.player_alive) return; // своих не боится, а следует за ними в update_free_motion

    const float dist = glm::length(context.player_position - m_mob.get_position());
    if (dist < 4.0f && !context.player_sneaking) {
        m_ai.add_fear(50.0f * dt, context.player_position);
        if (m_ai.get_fear() > 40.0f && !Horse_AI::is_urgent(m_ai.get_state())) {
            m_ai.enter_state(Horse_State::Scared);
        }
    }
}

// ---------------------------------------------------------------------------------------
//  Движение без всадника
// ---------------------------------------------------------------------------------------
void Horse::update_free_motion(float dt, const Horse_Context& context) {
    const Horse_State state = m_ai.get_state();
    const glm::vec3 pos = m_mob.get_position();

    glm::vec3 desired(0.0f);
    float turn_rate = 2.5f;

    if (state != m_last_state) {
        m_has_wander_target = false;
        m_last_state = state;
    }

    // Хозяйка рядом — подходит и смотрит на него (как в гайде, раздел 4).
    const glm::vec3 to_player = context.player_position - pos;
    const float player_dist = glm::length(glm::vec3(to_player.x, 0.0f, to_player.z));
    const bool follow_owner = m_tamed && context.player_alive && player_dist < 8.0f &&
                              (state == Horse_State::Idle || state == Horse_State::Wander || state == Horse_State::Happy);

    const bool seeking = m_seek_timer > 0.0f;
    // Пауки рядом — бежит к ним (кроме паники и страха).
    const bool hunting = context.prey_valid && state != Horse_State::Flee && state != Horse_State::Scared;
    if (seeking) {
        const glm::vec3 to_mate(m_seek_target.x - pos.x, 0.0f, m_seek_target.z - pos.z);
        if (glm::length(to_mate) > 0.05f) {
            turn_towards(std::atan2(to_mate.x, to_mate.z), 3.5f, dt);
            desired = forward_of(m_yaw) * Config::horse_walk_speed * 1.8f;
        }
    } else if (hunting) {
        const glm::vec3 prey_center = 0.5f * (context.prey_min + context.prey_max);
        const glm::vec3 to_prey(prey_center.x - pos.x, 0.0f, prey_center.z - pos.z);
        if (glm::length(to_prey) > 0.05f) {
            turn_towards(std::atan2(to_prey.x, to_prey.z), 4.0f, dt);
            desired = forward_of(m_yaw) * Config::horse_walk_speed * 2.0f;
        }
    } else if (follow_owner) {
        turn_towards(std::atan2(to_player.x, to_player.z), 3.0f, dt);
        if (player_dist > 2.5f) desired = forward_of(m_yaw) * 2.2f;
    } else if (state == Horse_State::Wander) {
        if (!m_has_wander_target) {
            m_wander_target = pos + glm::vec3(rand_range(-10.0f, 10.0f), 0.0f, rand_range(-10.0f, 10.0f));
            m_has_wander_target = true;
        }
        const glm::vec3 to_target = glm::vec3(m_wander_target.x - pos.x, 0.0f, m_wander_target.z - pos.z);
        if (glm::length(to_target) < 0.7f) {
            m_has_wander_target = false; // дошла — в следующем кадре выберет новую точку
        } else {
            turn_towards(std::atan2(to_target.x, to_target.z), turn_rate, dt);
            desired = forward_of(m_yaw) * Config::horse_walk_speed;
        }
    } else if (state == Horse_State::Flee || state == Horse_State::Scared) {
        glm::vec3 away = pos - m_ai.get_fear_source();
        away.y = 0.0f;
        if (glm::length(away) < 0.01f) away = forward_of(m_yaw);
        away = glm::normalize(away);
        turn_towards(std::atan2(away.x, away.z), state == Horse_State::Flee ? 5.0f : 3.0f, dt);
        desired = forward_of(m_yaw) * (state == Horse_State::Flee ? Config::horse_flee_speed : 2.0f);
    } else if (state == Horse_State::Idle) {
        // Оглядывается, как в гайде: лёгкое покачивание курса.
        m_yaw += std::sin(m_ai.get_state_time() * 0.7f) * 0.15f * dt;
    } else if (state == Horse_State::Happy) {
        // Радуется: подпрыгивает на месте.
        m_hop_timer -= dt;
        if (m_hop_timer <= 0.0f && m_on_ground) {
            m_vertical_velocity = 5.0f;
            m_on_ground = false;
            m_hop_timer = 0.8f;
        }
    }

    // Плавный разгон/торможение к желаемой скорости.
    const float k = 1.0f - std::exp(-6.0f * dt);
    m_ride_velocity += (desired - m_ride_velocity) * k;
    m_yaw_rate *= std::exp(-8.0f * dt);
}

// ---------------------------------------------------------------------------------------
//  Приручение (UpdateTaming из гайда, вариант с растущим шансом по числу попыток)
// ---------------------------------------------------------------------------------------
void Horse::update_taming_motion(float dt) {
    m_taming_time += dt;

    // Брыкается: рывки вперёд, случайные повороты, подскоки.
    m_yaw += rand_range(-1.0f, 1.0f) * 4.0f * dt;
    m_yaw_rate = rand_range(-3.0f, 3.0f);
    const float bucking_speed = 1.5f + 1.5f * (0.5f + 0.5f * std::sin(m_taming_time * 9.0f));
    m_ride_velocity = forward_of(m_yaw) * bucking_speed;
    if (m_on_ground && rand01() < 1.6f * dt) {
        m_vertical_velocity = 4.5f;
        m_on_ground = false;
    }

    // Игрока трясёт.
    m_seat_shake = glm::vec3(rand_range(-0.07f, 0.07f), rand_range(-0.05f, 0.05f), rand_range(-0.07f, 0.07f));

    m_taming_progress += dt / Config::horse_taming_seconds;

    // Рывок-попытка сбросить: начинается не сразу, чтобы у игрока был шанс удержаться.
    if (m_taming_time > 1.0f && rand01() < (0.4f + m_taming_progress * 0.3f) * dt) {
        if (rand01() < taming_success_chance(m_taming_attempts)) {
            finish_taming();
        } else {
            ++m_taming_attempts;
            m_throw_request = true;
        }
        return;
    }

    if (m_taming_progress >= 1.0f) finish_taming();
}

// ---------------------------------------------------------------------------------------
//  Езда + дрифт
// ---------------------------------------------------------------------------------------
void Horse::update_riding_motion(float dt, const Horse_Ride_Input& input) {
    m_seat_shake = glm::vec3(0.0f);

    // Характер лошади влияет на управление: уставшая медленнее разгоняется, напуганная хуже слушается.
    const float energy_factor = 0.5f + 0.5f * (m_ai.get_energy() / 100.0f);
    const float fear_factor = 1.0f - 0.5f * (m_ai.get_fear() / 100.0f);

    // Прыжок всадника: нажатие пробела на земле.
    if (input.jump && m_on_ground) {
        m_vertical_velocity = Config::horse_ride_jump_speed;
        m_on_ground = false;
    }

    // Бег: клик Ctrl переключает; без W бег сам выключается (бегут только вперёд).
    if (input.sprint_toggle) m_sprinting = !m_sprinting;
    if (input.throttle <= 0.0f) m_sprinting = false;

    const float speed_before = glm::length(m_ride_velocity);
    const bool can_drift = input.handbrake && speed_before > Config::horse_drift_min_speed;

    // --- Руль: нос поворачивается к желаемому курсу; на ручнике поворот резче ---
    float applied_turn = 0.0f;
    if (glm::length(input.wish_dir) > 0.01f) {
        const float target_yaw = std::atan2(input.wish_dir.x, input.wish_dir.z);
        const float diff = wrap_pi(target_yaw - m_yaw);
        float turn_rate = Config::horse_ride_turn_rate * fear_factor / (1.0f + speed_before * 0.06f);
        if (can_drift) turn_rate *= Config::horse_drift_turn_multiplier;
        applied_turn = std::clamp(diff, -turn_rate * dt, turn_rate * dt);
        m_yaw += applied_turn;
    }
    m_yaw_rate = dt > 0.0f ? applied_turn / dt : 0.0f;

    // --- Разложение скорости на «вдоль носа» и боковую ---
    const glm::vec3 forward = forward_of(m_yaw);
    float speed_along = glm::dot(m_ride_velocity, forward);
    glm::vec3 lateral = m_ride_velocity - forward * speed_along;

    // --- Газ / тормоз ---
    const float max_forward = Config::horse_ride_max_speed * energy_factor *
                              (m_sprinting ? Config::horse_sprint_speed_multiplier : 1.0f);
    const float accel_scale = input.handbrake ? 0.5f : 1.0f;
    if (input.throttle > 0.0f) {
        if (speed_along < max_forward) {
            speed_along = std::min(speed_along + Config::horse_ride_accel * energy_factor * accel_scale * dt, max_forward);
        }
    } else if (input.throttle < 0.0f) {
        if (speed_along > 0.0f) {
            speed_along = std::max(0.0f, speed_along - Config::horse_ride_brake * dt);
        } else {
            speed_along = std::max(speed_along - Config::horse_ride_accel * 0.5f * dt, -Config::horse_ride_reverse_speed);
        }
    } else {
        speed_along *= std::exp(-2.5f * dt); // отпустила газ — сама замедляется
    }
    if (input.handbrake) speed_along *= std::exp(-0.25f * dt); // дрифт понемногу гасит скорость

    // --- Сцепление: боковое скольжение перетекает в движение вперёд (сохраняя почти всю скорость) ---
    const float grip = can_drift || (input.handbrake && glm::length(lateral) > 1.0f)
                           ? Config::horse_drift_grip : Config::horse_ride_grip;
    const float grip_k = 1.0f - std::exp(-grip * dt);
    const glm::vec3 transferred = lateral * grip_k;
    lateral -= transferred;
    speed_along += (speed_along >= 0.0f ? 1.0f : -1.0f) * glm::length(transferred) * 0.85f;
    // Перетекание бокового скольжения в ход может слегка перебрать скорость — режем; на ручнике
    // допускаем небольшой перебор (+10%), чтобы дрифт не «душил» лошадь у потолка скорости.
    speed_along = std::clamp(speed_along, -Config::horse_ride_reverse_speed,
                             input.handbrake ? max_forward * 1.1f : max_forward);

    m_ride_velocity = forward * speed_along + lateral;
    m_drifting = (input.handbrake && speed_before > Config::horse_drift_min_speed * 0.75f) ||
                 glm::length(lateral) > 3.0f;

    // --- Усталость от езды ---
    const float sprint_drain = m_sprinting ? Config::horse_sprint_energy_multiplier : 1.0f;
    m_ai.drain_energy(glm::length(m_ride_velocity) * Config::horse_ride_energy_drain * sprint_drain * dt);
}

// ---------------------------------------------------------------------------------------
//  Физика: коллизии, гравитация, прыжок через уступ
// ---------------------------------------------------------------------------------------
void Horse::integrate(float dt, const Chunk_Manager& chunk_manager) {
    glm::vec3 position = m_mob.get_position();

    // Вода: лошадь плывёт (всплывает), течение сносит её, воздух тратится под водой.
    const Liquid_Physics::Sample wet = Liquid_Physics::sample(chunk_manager, position, Config::horse_height);
    if (const int events = m_breath.tick(dt, wet.head_in_liquid, Config::breath_max_seconds)) {
        take_drowning_damage(static_cast<float>(events) * Config::drown_damage);
    }
    if (wet.body_in_liquid) {
        m_vertical_velocity = Liquid_Physics::swim_vertical_velocity(wet, true, false, m_vertical_velocity, dt);
    } else {
        m_vertical_velocity = std::max(Config::mob_max_fall_speed, m_vertical_velocity + Config::mob_gravity * dt);
    }

    glm::vec3 horizontal = m_ride_velocity + m_knockback;
    m_knockback *= std::exp(-6.0f * dt);
    if (glm::length(m_knockback) < 0.05f) m_knockback = glm::vec3(0.0f);
    horizontal = horizontal * Liquid_Physics::horizontal_drag(wet) + wet.flow;

    bool blocked = false;
    glm::vec3 target = position;
    target.x += horizontal.x * dt;
    if (!collides_at(target, chunk_manager)) {
        position.x = target.x;
    } else if (std::abs(horizontal.x) > 0.01f) {
        blocked = true;
        m_ride_velocity.x = 0.0f;
    }

    target = position;
    target.z += horizontal.z * dt;
    if (!collides_at(target, chunk_manager)) {
        position.z = target.z;
    } else if (std::abs(horizontal.z) > 0.01f) {
        blocked = true;
        m_ride_velocity.z = 0.0f;
    }

    // Упёрлась в уступ в один блок на ходу — перепрыгивает (раньше лошадь застревала у каждой ступеньки).
    if (blocked && m_on_ground && glm::length(horizontal) > 0.8f && !m_dying) {
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
}

// ---------------------------------------------------------------------------------------
//  Главный апдейт
// ---------------------------------------------------------------------------------------
void Horse::update(float dt, const Chunk_Manager& chunk_manager,
                   const Horse_Context& context, const Horse_Ride_Input& ride_input) {
    if (m_dying) {
        m_death_timer += dt;
        const float fall_t = std::clamp(m_death_timer / (Config::horse_death_seconds * 0.75f), 0.0f, 1.0f);
        m_mob.set_roll_pivot_height(Config::horse_width * 0.5f);
        m_mob.set_roll_degrees(90.0f * fall_t * fall_t);
        m_mob.set_hurt_flash(0.8f);
        m_mob.update(dt);
        return;
    }

    m_invulnerable_timer = std::max(0.0f, m_invulnerable_timer - dt);
    m_attack_cooldown = std::max(0.0f, m_attack_cooldown - dt);
    m_hurt_flash_timer = std::max(0.0f, m_hurt_flash_timer - dt);
    m_mob.set_hurt_flash(m_hurt_flash_timer / 0.4f);
    m_breeding.update(dt);
    m_mob.set_scale(m_breeding.size_factor());
    m_seek_timer = std::max(0.0f, m_seek_timer - dt);

    // 1. Мозг.
    if (m_has_rider) {
        m_ai.update_rider(dt);
    } else {
        update_perception(dt, context);
        m_ai.update_free(dt);
    }

    // 2. Желаемое движение.
    if (m_has_rider) {
        if (m_ai.get_state() == Horse_State::Taming) update_taming_motion(dt);
        else update_riding_motion(dt, ride_input);
    } else {
        m_drifting = false;
        update_free_motion(dt, context);
    }

    // 2b. Удар по пауку, если он в досягаемости (без всадника, не в панике).
    if (!m_has_rider && context.prey_valid && !Horse_AI::is_urgent(m_ai.get_state())) {
        m_pending_attack = attack(context.prey_min, context.prey_max);
    }

    // 3. Физика мира.
    integrate(dt, chunk_manager);

    // 4. Визуал: курс и наклон корпуса (в повороте — внутрь, в дрифте — сильнее).
    m_mob.set_yaw_degrees(glm::degrees(m_yaw));
    const float lean_gain = m_drifting ? 9.0f : 4.0f;
    const float target_roll = m_has_rider ? -std::clamp(m_yaw_rate * lean_gain, -24.0f, 24.0f) : 0.0f;
    m_roll += (target_roll - m_roll) * (1.0f - std::exp(-10.0f * dt));
    m_mob.set_roll_pivot_height(Config::horse_height * 0.5f);
    m_mob.set_roll_degrees(m_roll);

    // Клипы, если модель их содержит (horse.glb пока статичная — тогда ничего не происходит).
    const char* animation = animation_name_for_state(m_ai.get_state());
    if (m_mob.has_animation(animation)) m_mob.play_animation(animation);
    m_mob.update(dt);
}

void Horse::render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const {
    m_mob.render(shader, view, projection, ambient_intensity);
}
