#ifndef OPTICRAFT_KNIGHT_AI_H
#define OPTICRAFT_KNIGHT_AI_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <glm/glm.hpp>
#include "mobs/Mob.h"
#include "entities/effects/Status_Effects.h"

class Chunk_Manager;
class Shader;

// Данные игрока, необходимые рыцарю. Не зависит от Player напрямую.
struct Knight_Player_View {
    glm::vec3 position{0.0f};       // ноги игрока
    glm::vec3 velocity{0.0f};       // скорость игрока для прогнозирования
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    float eye_height = 1.62f;
    bool alive = true;
};

struct Knight_Tick_Result {
    int player_damage = 0;
};

enum class Knight_Attack : uint8_t {
    None = 0,
    Swipe,
    Double_Swipe,
    Ground_Slam,
    Charge,
    Leap_Strike,
    Shockwave,
    Spin_Attack,
    Rage_Strike,
    Count
};

enum class Knight_State : uint8_t {
    Dormant,
    Chase,
    Windup,
    Attacking,
    Jumping,
    Recovery,
    Returning,
    Dead
};

// Долговременные стратегии, которые рыцарь распознаёт по повторяющимся
// манёврам/действиям. Это не описание того, что игрок делает в данный кадр.
enum class Knight_Player_Pattern : uint8_t {
    None = 0,
    Rear_Flank,
    Orbit_Left,
    Orbit_Right,
    Kiting,
    Repeated_Jump,
    Melee_Spam,
    Straight_Rush,
    Count
};

// Полноценный босс-рыцарь. Модель обязательна: assets/mobs/knight.glb.
// Размер арены: 5x5 чанков x 3 чанка высотой (32 блока на чанк).
class Knight_Ai {
public:
    static constexpr const char* MODEL_PATH = "assets/mobs/knight.glb";
    static constexpr float CHUNK_SIZE = 32.0f;
    static constexpr float ARENA_WIDTH = 5.0f * CHUNK_SIZE;
    static constexpr float ARENA_HEIGHT = 3.0f * CHUNK_SIZE;
    static constexpr float MAX_HEALTH = 1200.0f;

    Knight_Ai(std::string name, const glm::vec3& spawn_position);

    bool load_model();
    void face_towards(const glm::vec3& target);
    Knight_Tick_Result update(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks);
    void render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient) const;

    bool take_damage(float amount, const glm::vec3& attacker_position);
    // Вызывается только после реально зарегистрированного попадания игрока.
    void observe_player_hit(const glm::vec3& attacker_position);
    bool kill();
    void apply_effect(Status_Effect effect, float duration, int amplifier) { m_effects.apply(effect, duration, amplifier); }

    const std::string& name() const { return m_name; }
    const glm::vec3& get_position() const { return m_position; }
    float get_health() const { return m_health; }
    float get_health_percent() const { return m_health / MAX_HEALTH; }
    int get_phase() const { return m_phase; }
    Knight_State get_state() const { return m_state; }
    Knight_Attack get_current_attack() const { return m_current_attack; }
    Knight_Player_Pattern get_learned_pattern() const { return m_learned_pattern; }
    float get_learning_confidence() const { return m_learning_confidence; }
    float get_pattern_evidence() const;
    bool is_dying() const { return m_dying; }
    bool is_removable() const { return m_dying && m_death_timer >= 3.0f; }

    void compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const;

private:
    struct Arena {
        glm::vec3 center{0.0f};
        float floor_y = 0.0f;
        float half_x = ARENA_WIDTH * 0.5f;
        float half_z = ARENA_WIDTH * 0.5f;
        float corner_cut = 18.0f;

        bool contains(const glm::vec3& p, float margin = 0.0f) const;
        glm::vec3 clamp(const glm::vec3& p, float margin = 0.0f) const;
    };

    std::string m_name;
    Mob m_model;
    Status_Effects m_effects;
    Arena m_arena;
    glm::vec3 m_position{0.0f};
    glm::vec3 m_velocity{0.0f};
    glm::vec3 m_last_seen_position{0.0f};
    glm::vec3 m_attack_aim{0.0f};
    float m_yaw = 0.0f;
    float m_health = MAX_HEALTH;
    float m_hurt_timer = 0.0f;
    float m_death_timer = 0.0f;
    float m_state_timer = 0.0f;
    float m_attack_cooldown = 0.0f;
    float m_jump_cooldown = 0.0f;
    float m_lost_target_timer = 0.0f;
    float m_clock = 0.0f;
    int m_phase = 1;
    Knight_State m_state = Knight_State::Dormant;
    Knight_Attack m_current_attack = Knight_Attack::None;
    Knight_Attack m_last_attack = Knight_Attack::None;
    bool m_has_target = false;
    bool m_grounded = true;
    bool m_hit_primary = false;
    bool m_hit_secondary = false;
    bool m_hit_tertiary = false;
    bool m_pending_leap_impact = false;
    bool m_dying = false;
    std::string m_current_animation;

    // Долговременная память. Evidence растёт только после завершённого
    // поведенческого эпизода, а не каждый кадр нахождения игрока с одной стороны.
    static constexpr std::size_t PATTERN_COUNT = static_cast<std::size_t>(Knight_Player_Pattern::Count);
    std::array<float, PATTERN_COUNT> m_pattern_evidence{};
    Knight_Player_Pattern m_learned_pattern = Knight_Player_Pattern::None;
    Knight_Player_Pattern m_candidate_pattern = Knight_Player_Pattern::None;
    float m_learning_confidence = 0.0f;
    int m_learned_flank_direction = 0;
    float m_learning_timer = 0.0f;
    float m_episode_cooldown = 0.0f;
    float m_candidate_duration = 0.0f;
    float m_candidate_gap_time = 0.0f;
    float m_candidate_angular_travel = 0.0f;
    float m_candidate_distance_travel = 0.0f;
    int m_candidate_direction = 0;
    bool m_previous_player_airborne = false;
    float m_jump_pattern_window = 0.0f;
    int m_jump_pattern_events = 0;
    float m_player_hit_event_cooldown = 0.0f;

    static constexpr float BODY_HALF_X = 1.15f;
    static constexpr float BODY_HALF_Z = 0.62f;
    static constexpr float BODY_HEIGHT = 3.18f;
    static constexpr float GRAVITY = 38.0f;

    bool has_line_of_sight(const Knight_Player_View& player, const Chunk_Manager& chunks) const;
    bool can_see_player(const Knight_Player_View& player, const Chunk_Manager& chunks) const;
    bool can_occupy(const glm::vec3& feet_position, const Chunk_Manager& chunks) const;
    bool has_ground_below(const glm::vec3& feet_position, const Chunk_Manager& chunks) const;
    void update_learning(const Knight_Player_View& player, float dt);
    void record_pattern_episode(Knight_Player_Pattern pattern, int direction = 0);
    void refresh_learned_pattern();
    void reset_pattern_candidate();
    glm::vec3 predict_player(const Knight_Player_View& player, float seconds) const;
    void rotate_towards(const glm::vec3& target, float dt, float multiplier = 1.0f);
    bool facing_player(const Knight_Player_View& player) const;
    bool can_see_at_impact(const Knight_Player_View& player, const Chunk_Manager& chunks) const;
    void set_animation(const char* name, bool loop = true);
    void update_phase();
    void update_movement(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks);
    void move_towards(const glm::vec3& target, float speed, float dt, const Chunk_Manager& chunks);
    void begin_jump(const glm::vec3& target, int phase, bool attack_landing);
    void update_jump(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks, Knight_Tick_Result& result);
    void start_attack(Knight_Attack attack, const Knight_Player_View& player);
    Knight_Attack choose_attack(const Knight_Player_View& player, const Chunk_Manager& chunks);
    bool attack_is_available(Knight_Attack attack, float distance) const;
    void update_windup(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks);
    void update_attack(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks, Knight_Tick_Result& result);
    void apply_attack_hit(const Knight_Player_View& player, const Chunk_Manager& chunks, float range,
                          float damage, float arc_degrees, Knight_Tick_Result& result);
    void update_recovery(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks);
    void update_ai(float dt, const Knight_Player_View& player, const Chunk_Manager& chunks, Knight_Tick_Result& result);
};

#endif // OPTICRAFT_KNIGHT_AI_H
