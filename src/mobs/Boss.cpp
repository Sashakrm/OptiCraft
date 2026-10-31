#include "mobs/Boss.h"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

const Boss_Stats& boss_stats(Boss_Kind kind) {
    static const Boss_Stats colossus{"Stone Colossus", "assets/mobs/zombie.glb", 200.0f, 2.5f};
    static const Boss_Stats stalker{"Shadow Stalker", "assets/mobs/skeleton.glb", 120.0f, 1.8f};
    static const Boss_Stats knight{"Knight", "assets/mobs/knight.glb", 1200.0f, 1.0f};
    if (kind == Boss_Kind::Shadow_Stalker) return stalker;
    if (kind == Boss_Kind::Knight) return knight;
    return colossus;
}

namespace {
    // Фазы Stone_Colossus: 0 — идёт к игроку, 1 — замах, 2 — отдышка после удара.
    constexpr int kColossusAdvance = 0;
    constexpr int kColossusWindup = 1;
    constexpr int kColossusRecover = 2;
    constexpr float kColossusWindupSeconds = 1.0f;
    constexpr float kColossusRecoverSeconds = 3.0f;
    constexpr float kColossusSpeed = 1.8f;
    constexpr float kColossusReach = 3.0f;      // на таком расстоянии начинает замах
    constexpr float kColossusSlamRadius = 4.5f; // удар по площади
    constexpr float kColossusSlamDamage = 10.0f;

    // Фазы Shadow_Stalker: 0 — ждёт и телепортируется за спину, 1 — бьёт.
    constexpr int kStalkerWait = 0;
    constexpr int kStalkerStrike = 1;
    constexpr float kStalkerWaitSeconds = 5.0f;
    constexpr float kStalkerStrikeSeconds = 3.0f;
    constexpr float kStalkerStrikeInterval = 1.0f;
    constexpr float kStalkerReach = 2.2f;
    constexpr float kStalkerDamage = 4.0f;
    constexpr float kStalkerBehindDistance = 2.0f;

    constexpr float kHurtFlashSeconds = 0.4f;
    constexpr float kDeathSeconds = 3.0f;
}

Boss::Boss(Boss_Kind kind, std::string name, const glm::vec3& spawn_position)
    : m_kind(kind)
    , m_name(std::move(name))
    , m_position(spawn_position)
    , m_health(boss_stats(kind).max_health) {
    m_mob.set_position(spawn_position);
    m_mob.set_scale(boss_stats(kind).scale);
}

bool Boss::load_model() {
    const bool ok = m_mob.load(boss_stats(m_kind).model_path);
    if (ok && m_mob.has_animation("Idle")) m_mob.play_animation("Idle");
    return ok;
}

void Boss::face_towards(const glm::vec3& target) {
    glm::vec3 d = target - m_position;
    d.y = 0.0f;
    if (glm::length(d) < 0.001f) return;
    m_mob.set_yaw_degrees(glm::degrees(std::atan2(d.x, d.z)));
}

Boss_Tick_Result Boss::update(float dt, const Boss_Player_View& player) {
    Boss_Tick_Result result;
    m_hurt_timer = std::max(0.0f, m_hurt_timer - dt);
    m_mob.set_hurt_flash(m_hurt_timer / kHurtFlashSeconds);
    if (m_dying) {
        m_death_timer += dt;
        m_mob.update(dt);
        return result;
    }
    if (!player.alive) {
        m_mob.update(dt);
        return result;
    }
    switch (m_kind) {
        case Boss_Kind::Stone_Colossus: result = update_stone_colossus(dt, player); break;
        case Boss_Kind::Shadow_Stalker: result = update_shadow_stalker(dt, player); break;
        default: break;
    }
    m_mob.set_position(m_position);
    m_mob.update(dt);
    return result;
}

// Каменный колосс: медленно идёт на игрока; у самого игрока замахивается и бьёт по площади.
Boss_Tick_Result Boss::update_stone_colossus(float dt, const Boss_Player_View& player) {
    Boss_Tick_Result result;
    glm::vec3 to_player = player.position - m_position;
    to_player.y = 0.0f;
    const float dist = glm::length(to_player);
    m_timer += dt;

    switch (m_phase) {
        case kColossusAdvance:
            face_towards(player.position);
            if (dist > kColossusReach) {
                m_position += glm::normalize(to_player) * kColossusSpeed * dt;
            } else {
                m_phase = kColossusWindup;
                m_timer = 0.0f;
            }
            break;
        case kColossusWindup:
            face_towards(player.position);
            if (m_timer >= kColossusWindupSeconds) {
                if (dist <= kColossusSlamRadius) result.player_damage = static_cast<int>(kColossusSlamDamage);
                m_phase = kColossusRecover;
                m_timer = 0.0f;
            }
            break;
        case kColossusRecover:
            if (m_timer >= kColossusRecoverSeconds) {
                m_phase = kColossusAdvance;
                m_timer = 0.0f;
            }
            break;
        default:
            m_phase = kColossusAdvance;
            break;
    }
    return result;
}

// Теневой охотник: прячется, потом телепортируется за спину игроку и несколько секунд бьёт.
Boss_Tick_Result Boss::update_shadow_stalker(float dt, const Boss_Player_View& player) {
    Boss_Tick_Result result;
    m_timer += dt;

    if (m_phase == kStalkerWait) {
        if (m_timer >= kStalkerWaitSeconds) {
            glm::vec3 back = player.forward;
            back.y = 0.0f;
            if (glm::length(back) < 0.001f) back = glm::vec3(0.0f, 0.0f, 1.0f);
            back = glm::normalize(back);
            m_position = player.position - back * kStalkerBehindDistance;
            m_phase = kStalkerStrike;
            m_timer = 0.0f;
            m_mob.set_hurt_flash(0.0f);
        }
        return result;
    }

    // Фаза удара: бьёт раз в kStalkerStrikeInterval, если игрок в досягаемости.
    glm::vec3 to_player = player.position - m_position;
    to_player.y = 0.0f;
    face_towards(player.position);
    if (glm::length(to_player) <= kStalkerReach && std::fmod(m_timer, kStalkerStrikeInterval) < dt) {
        result.player_damage = static_cast<int>(kStalkerDamage);
    }
    if (m_timer >= kStalkerStrikeSeconds) {
        m_phase = kStalkerWait;
        m_timer = 0.0f;
    }
    return result;
}

bool Boss::take_damage(float amount, const glm::vec3& attacker_position) {
    (void)attacker_position;
    if (m_dying || amount <= 0.0f) return false;
    m_health -= amount;
    m_hurt_timer = kHurtFlashSeconds;
    if (m_health <= 0.0f) return kill();
    return false;
}

bool Boss::kill() {
    if (m_dying) return false;
    m_health = 0.0f;
    m_dying = true;
    m_death_timer = 0.0f;
    m_mob.set_roll_pivot_height(boss_stats(m_kind).scale * 0.5f);
    m_mob.set_roll_degrees(90.0f);
    return true;
}

void Boss::compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const {
    const float s = boss_stats(m_kind).scale;
    const float half = 0.6f * s;
    out_min = glm::vec3(m_position.x - half, m_position.y, m_position.z - half);
    out_max = glm::vec3(m_position.x + half, m_position.y + 2.0f * s, m_position.z + half);
}

void Boss::render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient) const {
    m_mob.render(shader, view, projection, ambient);
}
