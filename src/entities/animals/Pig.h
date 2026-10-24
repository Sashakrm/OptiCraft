//
// Pig — свинья в мире: склеивает визуал (Mob — модель/анимации/рендер) и
// поведение (Pig_AI — состояния голод/сон/счастье). Плюс простое блуждание
// по XZ, когда Pig_AI переходит в Walking.
//
// Гравитация и коллизии: свинья держит собственную вертикальную скорость и
// падает/стоит на блоках так же, как игрок (см. Player::update), но упрощённо —
// без прыжков, крауча и урона от падения. Chunk_Manager передаётся в update()
// как указатель (а не хранится внутри), потому что живёт дольше Pig и его
// проще прокинуть из Game::update_gameplay, где он уже есть под рукой.
//
#ifndef OPTICRAFT_PIG_H
#define OPTICRAFT_PIG_H

#include <glm/glm.hpp>
#include "entities/effects/Status_Effects.h"
#include <random>
#include "entities/animals/ai/Pig_AI.h"
#include "mobs/Mob.h"
#include "entities/animals/Breeding.h"
#include "physics/Liquid_Physics.h"
#include "world/blocks/Block_Types.h"
#include "utils/Config.h"

class Chunk_Manager;

class Pig {
public:
    // spawn_position — точка появления; name — только для логов Pig_AI ("Хрюша №3" и т.п.).
    Pig(std::string name, const glm::vec3& spawn_position);

    // Грузит (или берёт из общего кэша Mob) модель по пути — см. Mob::load.
    bool load_model(const std::string& glb_path);

    void update(float delta_time, const Chunk_Manager& chunk_manager);
    void render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const;

    const glm::vec3& get_position() const { return m_mob.get_position(); }
    const Pig_AI& get_ai() const { return m_ai; }

    // --- Бой -------------------------------------------------------------------------
    // Хитбокс (AABB) для попадания лучом из глаз игрока — те же габариты, что и у коллизии.
    glm::vec3 get_hitbox_min() const;
    glm::vec3 get_hitbox_max() const;
    void compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const;

    // Наносит урон и отбрасывает свинью от attacker_position; после удара она убегает
    // (Config::pig_panic_seconds). Возвращает true, если ЭТИМ ударом свинья умерла.
    // Повторный удар в пределах Config::pig_invulnerable_seconds игнорируется.
    bool take_damage(float amount, const glm::vec3& attacker_position);
    // Урон среды (утопление): без отбрасывания. Возвращает true, если умер.
    bool take_drowning_damage(float amount);
    float get_health() const { return m_health; }
    Status_Effects& effects() { return m_effects; }
    const Status_Effects& effects() const { return m_effects; }
    bool is_dying() const { return m_dying; }
    // Мгновенная смерть без дропа и без учёта неуязвимости (команда /killall).
    // Возвращает true, если свинья была жива.
    bool kill() {
        if (m_dying) return false;
        m_health = 0.0f;
        m_dying = true;
        m_death_timer = 0.0f;
        m_knockback = glm::vec3(0.0f);
        return true;
    }
    // --- Размножение -------------------------------------------------------------------
    static bool accepts_food(Block_Types item) { return item == Block_Types::Carrot; }
    Breeding_State& breeding() { return m_breeding; }
    const Breeding_State& breeding() const { return m_breeding; }
    bool is_baby() const { return m_breeding.is_baby(); }
    // Идти к точке (партнёру) — вызывается каждый кадр, пока ищет пару.
    void seek_towards(const glm::vec3& target);

    // Анимация смерти доиграна — свинью можно удалять из мира.
    bool is_removable() const { return m_dying && m_death_timer >= Config::pig_death_seconds; }

private:
    Pig_AI m_ai;
    Mob m_mob;
    std::mt19937 m_rng;

    // Здоровье и реакция на удары.
    float m_health = Config::pig_max_health;
    Status_Effects m_effects;
    float m_invulnerable_timer = 0.0f;
    float m_hurt_flash_timer = 0.0f;   // красная вспышка после удара
    float m_panic_timer = 0.0f;        // > 0 — убегает по m_walk_direction с повышенной скоростью
    glm::vec3 m_knockback{0.0f};       // горизонтальная скорость отбрасывания, затухает сама
    bool m_dying = false;
    float m_death_timer = 0.0f;
    Breeding_State m_breeding;
    float m_seek_timer = 0.0f;
    glm::vec3 m_seek_dir{0.0f};

    // Направление блуждания (единичный вектор по XZ), перевыбирается каждый
    // раз при входе в Pig_State::Walking.
    glm::vec3 m_walk_direction{1.0f, 0.0f, 0.0f};
    Pig_State m_last_applied_state = Pig_State::Idle; // чтобы play_animation/новое направление не дёргались каждый кадр

    // Вертикальная скорость свободного падения; сбрасывается в 0, как только
    // свинья касается земли — так же, как у игрока (Player::m_velocity.y).
    float m_vertical_velocity = 0.0f;
    bool m_on_ground = false;
    Liquid_Physics::Breath m_breath;   // свинья плавает (всплывает), воздух тратится под водой

    // Возвращает имя анимационного клипа для состояния ИИ — используется,
    // только если модель реально содержит клип с таким именем (см.
    // Mob::has_animation). Конвенция именования — как в README: "Idle",
    // "Walk", "Eat", "Sleep", "Happy" (Action'ы в Blender).
    static const char* animation_name_for_state(Pig_State state);

    void pick_new_walk_direction();

    // AABB-проверка занятости позиции твёрдым блоком — тот же принцип, что и
    // Player::collides_at, но с габаритами свиньи (Config::pig_width/pig_height).
    bool collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const;
};

#endif //OPTICRAFT_PIG_H
