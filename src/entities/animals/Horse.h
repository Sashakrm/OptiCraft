//
// Horse — лошадь в мире: визуал (Mob) + поведение (Horse_AI) + физика, приручение и езда с дрифтом.
//
// Схема из гайда по ИИ коня: три слоя —
//   1) свободная жизнь (Horse_AI: Idle/Wander/Graze/Happy/Scared/Sleep/Flee),
//   2) приручение (сел на дикую — она брыкается; удержался или повезло — приручена),
//   3) езда (Ridden): руль к направлению камеры, W/S — газ/тормоз, ПРОБЕЛ — ручник = дрифт.
//
// Модель horse.glb — статичная (без скелета и анимаций), поэтому «анимация» процедурная:
// наклон корпуса в повороте/дрифте (Mob::set_roll_degrees), подпрыгивание от радости, падение
// на бок при смерти. Если позже появится версия с клипами Idle/Walk/... — они подхватятся сами
// (как у свиньи, см. animation_name_for_state).
//
// Horse НЕ знает про Player/Input: Game передаёт контекст (где игрок) и ввод всадника, а после
// update() забирает get_seat_position() для игрока и флаг consume_throw_request() (сбросила ли
// всадника). Так физика лошади не зависит от устройства Player.
//
#ifndef OPTICRAFT_HORSE_H
#define OPTICRAFT_HORSE_H

#include <glm/glm.hpp>
#include <random>
#include "entities/animals/ai/Horse_AI.h"
#include "mobs/Mob.h"
#include "entities/animals/Breeding.h"
#include "world/blocks/Block_Types.h"
#include "utils/Config.h"

class Chunk_Manager;

// Что лошадь должна знать об игроке, когда на ней НЕ сидят (страх/слежение за хозяином).
struct Horse_Context {
    glm::vec3 player_position{0.0f};
    bool player_sneaking = false;   // крадущегося игрока дикая лошадь не пугается
    bool player_alive = true;
};

// Ввод всадника, уже переведённый Game в «язык лошади».
struct Horse_Ride_Input {
    glm::vec3 wish_dir{0.0f};  // горизонтальный единичный вектор желаемого курса; (0,0,0) — не рулить
    float throttle = 0.0f;     // +1 — W (газ), -1 — S (тормоз/задний ход)
    bool handbrake = false;    // пробел — ручник: сцепление падает, лошадь скользит боком (дрифт)
};

// Результат удара лошади. Horse не знает, кого бьёт (Player/моб): Game применяет результат сам,
// например player.take_damage(static_cast<int>(std::ceil(r.damage))) и импульс r.knockback.
struct Horse_Attack_Result {
    bool hit = false;                // false — удар не состоялся (кулдаун, мертва, далеко, ...)
    float damage = 0.0f;             // HP урона (0, если hit == false)
    glm::vec3 knockback{0.0f};       // импульс для цели (горизонталь + небольшой подброс)
};

class Horse {
public:
    Horse(std::string name, const glm::vec3& spawn_position);

    bool load_model(const std::string& glb_path);

    void update(float delta_time, const Chunk_Manager& chunk_manager,
                const Horse_Context& context, const Horse_Ride_Input& ride_input);
    void render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const;

    const glm::vec3& get_position() const { return m_mob.get_position(); }
    const Horse_AI& get_ai() const { return m_ai; }

    // --- Езда ------------------------------------------------------------------------
    bool has_rider() const { return m_has_rider; }
    bool is_tamed() const { return m_tamed; }
    bool is_drifting() const { return m_drifting; }
    // Скорость по земле, блоков/сек (для отладки/HUD).
    float get_speed() const { return glm::length(m_ride_velocity); }
    // Посадить всадника. false — уже занята или умирает. Дикая переходит в Taming, своя — в Ridden.
    bool mount();
    // Всадник слез (или Game снял его по другой причине — телепорт, смерть игрока).
    void dismount(const glm::vec3& rider_position);
    // true один раз, если лошадь только что сбросила всадника сама (провалила приручение) —
    // Game снимает игрока и зовёт dismount().
    bool consume_throw_request() { const bool r = m_throw_request; m_throw_request = false; return r; }
    // Куда сажать игрока (с лёгкой тряской, пока лошадь брыкается).
    glm::vec3 get_seat_position() const;
    float get_yaw_degrees() const { return m_mob.get_yaw_degrees(); }

    // --- Размножение и еда -----------------------------------------------------------
    // Лошади едят сырую конину, пшеницу и морковь.
    static bool accepts_food(Block_Types item) {
        return item == Block_Types::Raw_Horse_Meat || item == Block_Types::Wheat || item == Block_Types::Carrot;
    }
    Breeding_State& breeding() { return m_breeding; }
    const Breeding_State& breeding() const { return m_breeding; }
    bool is_baby() const { return m_breeding.is_baby(); }
    void seek_towards(const glm::vec3& target);
    // Накормить. Запоминает ПОСЛЕДНЮЮ съеденную еду (см. get_last_food) и, если можно, запускает
    // «любовь». true — еда принята (Game списывает предмет).
    bool feed(Block_Types food);
    // Последняя съеденная лошадью еда (Air — ещё ничего не ела). Нужна, чтобы знать, из-за чего
    // лошадь размножилась: конины или другого доступного ей питания.
    Block_Types get_last_food() const { return m_last_food; }
    // Что последними ели родители жеребёнка в момент зачатия (Air — дикая лошадь без родителей).
    Block_Types get_parent_food_a() const { return m_parent_food_a; }
    Block_Types get_parent_food_b() const { return m_parent_food_b; }
    void set_parent_foods(Block_Types a, Block_Types b) { m_parent_food_a = a; m_parent_food_b = b; }
    // Появился ли жеребёнок благодаря конине (хотя бы один из родителей последним ел её).
    bool born_from_horse_meat() const {
        return m_parent_food_a == Block_Types::Raw_Horse_Meat || m_parent_food_b == Block_Types::Raw_Horse_Meat;
    }

    // --- Бой -------------------------------------------------------------------------
    glm::vec3 get_hitbox_min() const;
    glm::vec3 get_hitbox_max() const;
    void compute_hitbox(glm::vec3& out_min, glm::vec3& out_max) const;
    bool take_damage(float amount, const glm::vec3& attacker_position);
    float get_health() const { return m_health; }
    bool is_dying() const { return m_dying; }
    bool kill() {
        if (m_dying) return false;
        m_health = 0.0f;
        m_dying = true;
        m_death_timer = 0.0f;
        m_knockback = glm::vec3(0.0f);
        return true;
    }
    // --- Нападение -------------------------------------------------------------------
    // Урон удара прямо сейчас: база + бонус за скорость (разгон/таран), не выше потолка.
    // Детёныш не бьёт.
    float compute_attack_damage() const;
    // Может ли лошадь ударить в этот момент (жива, не детёныш, кулдаун прошёл).
    bool can_attack() const;
    // Удар по цели с AABB [target_min, target_max]. Бьёт, если ближайшая точка бокса цели не
    // дальше Config::horse_attack_reach. При успехе запускает кулдаун и разворачивает лошадь к
    // цели. Ничего не применяет к самой цели — вернёт урон и импульс (см. Horse_Attack_Result).
    Horse_Attack_Result attack(const glm::vec3& target_min, const glm::vec3& target_max);
    // Удобная перегрузка для цели-точки (центр игрока и т.п.).
    Horse_Attack_Result attack(const glm::vec3& target_position);
    float get_attack_cooldown_remaining() const { return m_attack_cooldown; }
    bool is_removable() const { return m_dying && m_death_timer >= Config::horse_death_seconds; }

private:
    Horse_AI m_ai;
    Mob m_mob;
    std::mt19937 m_rng;

    float m_yaw = 0.0f; // радианы; вперёд = (sin yaw, 0, cos yaw), как у Mob (модель смотрит в +Z)

    // Характеристики/состояние.
    float m_health = Config::horse_max_health;
    float m_invulnerable_timer = 0.0f;
    float m_attack_cooldown = 0.0f;
    float m_hurt_flash_timer = 0.0f;
    glm::vec3 m_knockback{0.0f};
    bool m_dying = false;
    float m_death_timer = 0.0f;

    Breeding_State m_breeding;
    Block_Types m_last_food = Block_Types::Air;
    Block_Types m_parent_food_a = Block_Types::Air;
    Block_Types m_parent_food_b = Block_Types::Air;
    float m_seek_timer = 0.0f;
    glm::vec3 m_seek_target{0.0f};

    // Приручение.
    bool m_tamed = false;
    bool m_has_rider = false;
    bool m_throw_request = false;
    float m_taming_progress = 0.0f; // 0..1, пока игрок держится
    float m_taming_time = 0.0f;
    int m_taming_attempts = 0;
    glm::vec3 m_seat_shake{0.0f};

    // Физика.
    glm::vec3 m_ride_velocity{0.0f}; // горизонтальная скорость (XZ); общая и для ходьбы, и для езды
    float m_vertical_velocity = 0.0f;
    bool m_on_ground = false;
    bool m_drifting = false;
    float m_yaw_rate = 0.0f;   // рад/сек, для наклона корпуса
    float m_roll = 0.0f;       // градусы, сглаженный наклон
    float m_hop_timer = 0.0f;

    // Блуждание.
    glm::vec3 m_wander_target{0.0f};
    bool m_has_wander_target = false;
    Horse_State m_last_state = Horse_State::Idle;

    float rand01();
    float rand_range(float lo, float hi);
    bool collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const;

    void update_perception(float dt, const Horse_Context& context);
    void update_free_motion(float dt, const Horse_Context& context);
    void update_taming_motion(float dt);
    void update_riding_motion(float dt, const Horse_Ride_Input& input);
    // Двигает по m_ride_velocity с коллизиями, гравитацией и прыжком через уступ в 1 блок.
    void integrate(float dt, const Chunk_Manager& chunk_manager);
    void turn_towards(float target_yaw, float max_rate, float dt);
    void finish_taming();
    static const char* animation_name_for_state(Horse_State state);
};

#endif //OPTICRAFT_HORSE_H
