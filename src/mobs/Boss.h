//
// Legacy Boss — два ранних тестовых босса (Stone_Colossus и Shadow_Stalker).
// Полноценный трёхфазный босс-рыцарь реализован отдельно в Knight_Ai.h/.cpp и
// подключён к Mobs_Update. Boss_Kind::Knight оставлен как тип для общей команды /boss.
//
#ifndef OPTICRAFT_BOSS_H
#define OPTICRAFT_BOSS_H

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include "mobs/Mob.h"

enum class Boss_Kind : uint8_t {
    Stone_Colossus = 0,  // медленный, идёт на игрока и бьёт по площади
    Shadow_Stalker,      // телепортируется за спину игроку и быстро бьёт (legacy)
    Knight,              // полноценный трёхфазный босс-рыцарь (Knight_Ai)
    Count
};

struct Boss_Stats {
    const char* label;
    const char* model_path;
    float max_health;
    float scale;           // множитель модели (и хитбокса)
};

const Boss_Stats& boss_stats(Boss_Kind kind);

// Что боссу нужно знать об игроке в этом тике.
struct Boss_Player_View {
    glm::vec3 position{0.0f};  // ноги
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    bool alive = true;
};

// Что босс сделал за тик. Урон игроку — Mobs_Update передаст его дальше через Mob_Result.
struct Boss_Tick_Result {
    int player_damage = 0;
};

class Boss {
public:
    Boss(Boss_Kind kind, std::string name, const glm::vec3& spawn_position);

    bool load_model();
    Boss_Tick_Result update(float dt, const Boss_Player_View& player);
    void render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient) const;

    Boss_Kind kind() const { return m_kind; }
    const std::string& name() const { return m_name; }
    const glm::vec3& get_position() const { return m_position; }
    float get_health() const { return m_health; }

    void compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const;
    // Удар игрока. true — босс умер этим ударом.
    bool take_damage(float amount, const glm::vec3& attacker_position);
    bool kill();
    bool is_dying() const { return m_dying; }
    // Анимация смерти кончилась — босса можно убрать из мира.
    bool is_removable() const { return m_dying && m_death_timer >= 3.0f; }

private:
    Boss_Kind m_kind;
    std::string m_name;
    Mob m_mob;
    glm::vec3 m_position;
    float m_health;
    bool m_dying = false;
    float m_death_timer = 0.0f;
    float m_hurt_timer = 0.0f;

    // Общий таймер и фаза ИИ (смысл фаз — свой у каждого вида, см. Boss.cpp).
    float m_timer = 0.0f;
    int m_phase = 0;

    void face_towards(const glm::vec3& target);
    Boss_Tick_Result update_stone_colossus(float dt, const Boss_Player_View& player);
    Boss_Tick_Result update_shadow_stalker(float dt, const Boss_Player_View& player);
};

#endif //OPTICRAFT_BOSS_H
