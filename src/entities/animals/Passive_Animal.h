//
// Passive_Animal — пассивное животное, которое блуждает, пугается ударов и размножается.
// Сейчас это корова и овца: поведение у них одно, отличаются модель и числа (см. passive_stats()).
// Чтобы добавить ещё одно пассивное животное — добавить значение в Passive_Kind и строку в таблицу
// в Passive_Animal.cpp, без копирования логики.
//
// Модель грузится из assets/mobs/<model>.glb (см. passive_stats). Если модели нет — load_model()
// возвращает false, и Mobs_Update не спавнит этот вид (с паузой, чтобы не спамить лог).
//
#ifndef OPTICRAFT_PASSIVE_ANIMAL_H
#define OPTICRAFT_PASSIVE_ANIMAL_H

#include <glm/glm.hpp>
#include <random>
#include <string>
#include "entities/animals/Breeding.h"
#include "mobs/Mob.h"
#include "physics/Liquid_Physics.h"
#include "utils/Config.h"
#include "world/blocks/Block_Types.h"

class Chunk_Manager;

enum class Passive_Kind { Cow, Sheep };

// Числа одного вида. Берутся из Config в passive_stats(); здесь только форма данных.
struct Passive_Stats {
    const char* label;          // для логов и имён ("Cow", "Sheep")
    const char* model_path;     // путь к .glb
    int min_count;              // сколько держать рядом с игроком
    float spawn_radius_min;
    float spawn_radius_max;
    float walk_speed;
    float width;
    float height;
    float max_health;
    float panic_seconds;
    float panic_speed;          // «бег» — убегает после удара с этой скоростью
    float invulnerable_seconds;
    float death_seconds;
    float respawn_delay;
    Block_Types food;           // чем кормить (размножение)
    Block_Types drop_item;      // что выпадает при убийстве (Air — ничего)
    int drop_min;
    int drop_max;
    bool can_swim;              // плавает (всплывает) или тонет — см. Liquid_Physics
};

const Passive_Stats& passive_stats(Passive_Kind kind);

class Passive_Animal {
public:
    Passive_Animal(Passive_Kind kind, std::string name, const glm::vec3& spawn_position);

    Passive_Kind kind() const { return m_kind; }
    const Passive_Stats& stats() const { return passive_stats(m_kind); }

    bool load_model();
    void update(float delta_time, const Chunk_Manager& chunk_manager);
    void render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const;

    const glm::vec3& get_position() const { return m_mob.get_position(); }
    const std::string& get_name() const { return m_name; }

    glm::vec3 get_hitbox_min() const;
    glm::vec3 get_hitbox_max() const;
    void compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const;
    bool take_damage(float amount, const glm::vec3& attacker_position);
    // Урон среды (утопление): без отбрасывания. Возвращает true, если умер.
    bool take_drowning_damage(float amount);
    bool is_dying() const { return m_dying; }
    bool kill() {
        if (m_dying) return false;
        m_health = 0.0f; m_dying = true; m_death_timer = 0.0f; m_knockback = glm::vec3(0.0f);
        return true;
    }
    bool is_removable() const { return m_dying && m_death_timer >= stats().death_seconds; }

    bool accepts_food(Block_Types item) const { return item == stats().food; }
    Breeding_State& breeding() { return m_breeding; }
    const Breeding_State& breeding() const { return m_breeding; }
    bool is_baby() const { return m_breeding.is_baby(); }
    void seek_towards(const glm::vec3& target);

private:
    Passive_Kind m_kind;
    std::string m_name;
    Mob m_mob;
    std::mt19937 m_rng;
    Breeding_State m_breeding;

    float m_health;
    float m_invulnerable_timer = 0.0f;
    float m_hurt_flash_timer = 0.0f;
    float m_panic_timer = 0.0f;
    glm::vec3 m_knockback{0.0f};
    bool m_dying = false;
    float m_death_timer = 0.0f;

    // Блуждание: стоит / идёт по m_walk_direction, пока не истечёт таймер состояния.
    bool m_walking = false;
    float m_state_timer = 2.0f;
    glm::vec3 m_walk_direction{1.0f, 0.0f, 0.0f};
    float m_seek_timer = 0.0f;
    glm::vec3 m_seek_dir{0.0f};

    float m_vertical_velocity = 0.0f;
    bool m_on_ground = false;
    Liquid_Physics::Breath m_breath;

    void set_walking(bool walking);
    bool collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const;
};

#endif //OPTICRAFT_PASSIVE_ANIMAL_H