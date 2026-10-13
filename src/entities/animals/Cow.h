//
// Cow — корова: модель assets/mobs/cow.glb (Mob) + простое блуждание + физика как у свиньи.
// Ест пшеницу (кормёжка двух коров -> телёнок, см. Breeding.h и Game::update_breeding).
// Если у модели есть клипы "Idle"/"Walk" — переключаются сами.
//
#ifndef OPTICRAFT_COW_H
#define OPTICRAFT_COW_H

#include <glm/glm.hpp>
#include <random>
#include <string>
#include "entities/animals/Breeding.h"
#include "mobs/Mob.h"
#include "utils/Config.h"
#include "world/blocks/Block_Types.h"

class Chunk_Manager;

class Cow {
public:
    Cow(std::string name, const glm::vec3& spawn_position);

    bool load_model(const std::string& glb_path);
    void update(float delta_time, const Chunk_Manager& chunk_manager);
    void render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const;

    const glm::vec3& get_position() const { return m_mob.get_position(); }
    const std::string& get_name() const { return m_name; }

    glm::vec3 get_hitbox_min() const;
    glm::vec3 get_hitbox_max() const;
    void compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const;
    bool take_damage(float amount, const glm::vec3& attacker_position);
    bool is_dying() const { return m_dying; }
    bool kill() {
        if (m_dying) return false;
        m_health = 0.0f; m_dying = true; m_death_timer = 0.0f; m_knockback = glm::vec3(0.0f);
        return true;
    }
    bool is_removable() const { return m_dying && m_death_timer >= Config::cow_death_seconds; }

    static bool accepts_food(Block_Types item) { return item == Block_Types::Wheat; }
    Breeding_State& breeding() { return m_breeding; }
    const Breeding_State& breeding() const { return m_breeding; }
    bool is_baby() const { return m_breeding.is_baby(); }
    void seek_towards(const glm::vec3& target);

private:
    std::string m_name;
    Mob m_mob;
    std::mt19937 m_rng;
    Breeding_State m_breeding;

    float m_health = Config::cow_max_health;
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

    void set_walking(bool walking);
    bool collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const;
};

#endif //OPTICRAFT_COW_H
