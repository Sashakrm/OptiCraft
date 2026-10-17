//
// Created by noktemor on 15.10.2026.
//
//
// Hostile_Mob — враждебный моб: зомби и скелет. Поведение одно (бежать к игроку и бить в упор),
// отличаются модель и числа (см. hostile_stats()). Чтобы добавить ещё одного врага — добавить
// значение в Hostile_Kind и строку в таблицу в Hostile_Mob.cpp.
//
// Обнаружение делает Mobs_Update (дистанция + луч через блоки) и передаёт флаг player_seen в
// update(). Сам моб тревогу от товарищей получает через alert() — тогда он тоже бежит к игроку,
// даже если его самого игрок не видит. Так враги идут толпой.
//
// Модель: assets/mobs/zombie.glb или skeleton.glb. Ожидаются клипы Idle и Walk; Attack —
// необязательный (если его нет, удар просто происходит без анимации).
//
#ifndef OPTICRAFT_HOSTILE_MOB_H
#define OPTICRAFT_HOSTILE_MOB_H


#include <glm/glm.hpp>
#include <random>
#include <string>
#include "mobs/Mob.h"
#include "physics/Liquid_Physics.h"
#include "utils/Config.h"

class Chunk_Manager;

enum class Hostile_Kind { Zombie, Skeleton, Spider };

struct Hostile_Stats {
    const char* label;
    const char* model_path;
    int min_count;
    float spawn_radius_min;
    float spawn_radius_max;
    float walk_speed;       // блуждание
    float run_speed;        // погоня
    float detect_range;     // на таком расстоянии видит игрока (вдвое меньше, если он крадётся)
    float width;
    float height;
    float max_health;
    float attack_damage;    // HP за удар по игроку
    float attack_reach;     // горизонтальная дистанция до игрока, с которой бьёт
    float attack_cooldown;
    float invulnerable_seconds;
    float death_seconds;
    // Умеет плавать? Нет — тонет (без всплытия и без гребков), см. Liquid_Physics.
    bool can_swim;
};

const Hostile_Stats& hostile_stats(Hostile_Kind kind);

// Что врагу известно о мире в этом тике (заполняет Mobs_Update).
struct Hostile_Context {
    glm::vec3 player_position{0.0f};  // ноги игрока
    bool player_alive = true;
    bool player_seen = false;         // видит ли этот враг игрока прямо сейчас (дистанция и луч)
    float player_speed = 0.0f;        // горизонтальная скорость игрока, блоков/с: от неё растёт урон удара
    // false — игрока в этот момент не трогать (паук днём). Враги с обычной логикой — всегда true.
    bool player_targetable = true;
    // Добыча — лошадь (паук её бьёт). Если valid, враг гонится за ней, а не за игроком.
    bool prey_valid = false;
    glm::vec3 prey_position{0.0f};    // центр лошади
    glm::vec3 prey_min{0.0f};         // хитбокс лошади
    glm::vec3 prey_max{0.0f};
};

// Что враг сделал за тик: удар по игроку (урон до брони) и/или удар по лошади.
struct Hostile_Result {
    int player_damage = 0;
    bool hit_horse = false;
    float horse_damage = 0.0f;
};

class Hostile_Mob {
public:
    Hostile_Mob(Hostile_Kind kind, std::string name, const glm::vec3& spawn_position);

    Hostile_Kind kind() const { return m_kind; }
    const Hostile_Stats& stats() const { return hostile_stats(m_kind); }

    bool load_model();

    // Возвращает урон, который враг нанёс игроку в этом тике (0 — не ударил).
    Hostile_Result update(float delta_time, const Chunk_Manager& chunk_manager, const Hostile_Context& context);
    void render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const;

    // Тревога от товарища: видел игрока другой враг рядом — этот тоже идёт к нему.
    void alert(const glm::vec3& player_position);
    bool is_aggro() const { return m_memory_timer > 0.0f; }

    const glm::vec3& get_position() const { return m_mob.get_position(); }
    const std::string& get_name() const { return m_name; }
    // Точка, откуда враг смотрит (для проверки луча).
    glm::vec3 get_eye() const { return m_mob.get_position() + glm::vec3(0.0f, stats().height * 0.8f, 0.0f); }

    glm::vec3 get_hitbox_min() const;
    glm::vec3 get_hitbox_max() const;
    void compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const;
    // Удар игрока. Враг после удара начинает преследовать атакующего.
    bool take_damage(float amount, const glm::vec3& attacker_position);
    // Урон среды (утопление): без отбрасывания и без злости на кого-то. Возвращает true, если умер.
    bool take_drowning_damage(float amount);
    bool is_dying() const { return m_dying; }
    bool kill() {
        if (m_dying) return false;
        m_health = 0.0f; m_dying = true; m_death_timer = 0.0f; m_knockback = glm::vec3(0.0f);
        return true;
    }
    bool is_removable() const { return m_dying && m_death_timer >= stats().death_seconds; }

private:
    Hostile_Kind m_kind;
    std::string m_name;
    Mob m_mob;
    std::mt19937 m_rng;

    float m_health;
    float m_invulnerable_timer = 0.0f;
    float m_hurt_flash_timer = 0.0f;
    glm::vec3 m_knockback{0.0f};
    bool m_dying = false;
    float m_death_timer = 0.0f;

    // Погоня: помнит игрока memory_seconds после того, как потеряла его из виду.
    float m_memory_timer = 0.0f;
    glm::vec3 m_target_position{0.0f};
    float m_attack_cooldown = 0.0f;

    // Обход препятствий: если погоня не сдвинула врага почти никуда (он упёрся в блоки), на
    // короткое время он уходит боком. Без поиска пути это единственный способ выбраться из ямы.
    float m_stuck_check_timer = 0.0f;
    glm::vec3 m_stuck_check_position{0.0f};
    float m_detour_timer = 0.0f;
    glm::vec3 m_detour_direction{0.0f};

    // Блуждание, когда никого не преследует.
    bool m_walking = false;
    float m_state_timer = 2.0f;
    glm::vec3 m_walk_direction{1.0f, 0.0f, 0.0f};

    float m_vertical_velocity = 0.0f;
    bool m_on_ground = false;
    Liquid_Physics::Breath m_breath;

    void set_walking(bool walking);
    void play_clip(const char* name, bool loop = true);
    bool collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const;
};


#endif //OPTICRAFT_HOSTILE_MOB_H
