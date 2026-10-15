//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_PLAYER_H
#define OPTICRAFT_PLAYER_H


#include <glm/glm.hpp>
#include "input/Input_Manager.h"
#include "world/chunks/Chunk_Manager.h"
#include "physics/Liquid_Physics.h"

class Player {
private:
    glm::vec3 m_position;
    glm::vec3 m_velocity;
    glm::vec3 m_size; // Ширина, высота, глубина

    bool m_on_ground;
    float m_eye_height;

    // Физика
    float m_move_speed;
    float m_jump_force;
    float m_gravity;

    // Полёт
    bool m_is_flying = false;
    float m_fly_speed = Config::player_fly_speed;

    // Верховая езда: пока true, Player не двигает себя сам — позицию каждый кадр выставляет
    // лошадь (см. Mobs_Update::update_frame), физика/гравитация/падение отключены.
    bool m_is_riding = false;

    // Присед
    bool m_is_crouching = false;
    float m_standing_height;
    float m_standing_eye_height;

    // Здоровье
    int m_health;
    int m_max_health;

    // Голод (Survival). Модель как в Minecraft 1.8: еда (0..max) + насыщение + истощение,
    // см. константы в Config.h и update_hunger(). В Creative не тратится (m_hunger_enabled).
    int m_food_level = Config::player_max_food;
    float m_saturation = Config::player_initial_saturation;
    float m_exhaustion = 0.0f;
    float m_hunger_regen_timer = 0.0f;
    float m_heal_fraction = 0.0f;   // дробная часть лечения от быстрого восстановления (HP целые)
    float m_starvation_timer = 0.0f;
    bool m_hunger_enabled = false;

    // Вода и воздух. m_breath — сколько воздуха осталось под водой; m_head_underwater — голова в
    // жидкости прямо сейчас (для HUD и для тонущих мобов). Урон среды (падение, утопление) включается
    // одним флагом m_fall_damage_enabled — он же означает "правила Survival".
    Liquid_Physics::Breath m_breath;
    bool m_head_underwater = false;
    bool m_in_liquid = false;
    void tick_breath(float delta_time, bool head_in_liquid);

    // Импульс от инструментов (см. apply_impulse): горизонтальная скорость, которая затухает со временем.
    // Прибавляется к движению каждый кадр, останавливается о стену.
    glm::vec3 m_impulse{0.0f};

    // Падение (см. take_damage от удара о землю после update_crouch/движения по Y ниже).
    // "Было ли на земле в начале кадра" + наивысшая точка с начала падения — чтобы посчитать
    // дистанцию падения к моменту приземления. Урон применяется только если m_fall_damage_enabled
    // (включается извне для Survival — см. set_fall_damage_enabled).
    bool m_fall_damage_enabled = false;
    float m_fall_start_height = 0.0f;
    void apply_fall_damage();

    // Ввод
    Input_Manager::Move_Input m_pending_input;
    glm::vec3 m_view_forward;
    glm::vec3 m_view_right;

    bool collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const;
    bool is_solid_block_at(const glm::vec3& world_pos, const Chunk_Manager& chunk_manager) const;

    bool would_fit_at_height(float height, const Chunk_Manager& chunk_manager) const;
    void update_crouch(const Chunk_Manager& chunk_manager);

public:
    Player();

    // Обновление
    void update(float delta_time, const Chunk_Manager& chunk_manager);
    void set_movement_input(const Input_Manager::Move_Input& input,
                            const glm::vec3& view_forward,
                            const glm::vec3& view_right);

    // Полёт
    void toggle_flying() { m_is_flying = !m_is_flying; m_velocity = glm::vec3(0.0f); }
    bool is_flying() const { return m_is_flying; }

    // Верховая езда
    bool is_riding() const { return m_is_riding; }
    void set_riding(bool riding) {
        m_is_riding = riding;
        m_velocity = glm::vec3(0.0f);
        m_fall_start_height = m_position.y; // падение с седла не считается
        m_on_ground = riding;
        m_is_flying = false;
    }

    // Присед
    bool is_crouching() const { return m_is_crouching; }

    // Здоровье
    int get_health() const { return m_health; }
    int get_max_health() const { return m_max_health; }
    bool is_alive() const { return m_health > 0; }
    void take_damage(int amount);
    void heal(int amount);

    // Голод. update_hunger зовётся каждый кадр из Game (только в Survival и пока игрок жив):
    // переводит истощение в потерю насыщения/еды, лечит при сытости и бьёт при полном голоде.
    void set_hunger_enabled(bool enabled) { m_hunger_enabled = enabled; }
    bool is_hunger_enabled() const { return m_hunger_enabled; }
    void update_hunger(float delta_time);
    // Истощение копится от действий (ходьба, прыжок, добыча, удар...). В Creative игнорируется.
    void add_exhaustion(float amount);
    int get_food_level() const { return m_food_level; }
    int get_max_food() const { return Config::player_max_food; }
    float get_saturation() const { return m_saturation; }
    // Есть только пока не сыт (как в Minecraft): при полной еде ПКМ с едой ничего не делает.
    bool can_eat() const { return m_food_level < Config::player_max_food; }
    // Съеденный предмет: +очки еды (до максимума) и +насыщение (не выше текущей еды).
    void eat(int food_points, float saturation);

    // Урон от падения (Survival) — вкл/выкл извне в зависимости от текущего режима игры.
    void set_fall_damage_enabled(bool enabled) { m_fall_damage_enabled = enabled; }
    // Сбрасывает отсчёт высоты падения к текущей позиции — вызывать при включении Survival,
    // чтобы не получить урон за падение, случившееся ещё в Creative/полёте.
    void reset_fall_tracking() { m_fall_start_height = m_position.y; }
    // Гасит текущую скорость — нужно после телепорта, чтобы игрок не "долетел" со старым импульсом.
    void reset_velocity() { m_velocity = glm::vec3(0.0f); m_impulse = glm::vec3(0.0f); }

    // Импульс от инструмента: direction — направление (нормализуется), speed — блоков/с.
    // Горизонталь уходит в m_impulse и затухает; вертикаль даётся сразу скоростью прыжка.
    void apply_impulse(const glm::vec3& direction, float speed);
    // Горизонтальная скорость тела (включая импульс и течение) — от неё зависит урон удара.
    float get_horizontal_speed() const { return glm::length(glm::vec2(m_velocity.x, m_velocity.z)); }

    // Воздух (Survival): сколько секунд осталось и голова ли под водой (для HUD).
    float get_air() const { return m_breath.air; }
    float get_max_air() const { return Config::breath_max_seconds; }
    bool is_head_underwater() const { return m_head_underwater; }
    // Тело в жидкости (центр) — для HUD и физики.
    bool is_in_liquid() const { return m_in_liquid; }

    // Смерть: полное восстановление здоровья и телепорт в заданную точку (точка спавна мира).
    void respawn(const glm::vec3& position);

    // Коллизии (упрощённые)
    bool check_collision(const glm::vec3& new_pos, const Chunk_Manager& chunk_manager) const;
    bool intersects_block_aabb(int block_x, int block_y, int block_z) const;

    // Геттеры/Сеттеры
    const glm::vec3& get_position() const { return m_position; }
    void set_position(const glm::vec3& pos) { m_position = pos; }
    float get_eye_height() const { return m_eye_height; }
    float get_height() const { return m_size.y; }
    bool is_on_ground() const { return m_on_ground; }
};

#endif //OPTICRAFT_PLAYER_H
