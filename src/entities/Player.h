//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_PLAYER_H
#define OPTICRAFT_PLAYER_H


#include <glm/glm.hpp>
#include "input/Input_Manager.h"
#include "world/Chunk_Manager.h"
#include "Camera.h"

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

    // Ввод
    Input_Manager::Move_Input m_pending_input;

public:
    Player();

    // Обновление
    void update(float delta_time, const Chunk_Manager& chunk_manager, const Camera* camera);
    void set_movement_input(const Input_Manager::Move_Input& input);

    // Коллизии
    bool check_block_collision(const glm::vec3& new_pos, const Chunk_Manager& chunk_manager) const;
    glm::vec3 resolve_collision(const glm::vec3& new_pos, const Chunk_Manager& chunk_manager) ;
    Block_Type get_block_at(const glm::vec3& world_pos, const Chunk_Manager& chunk_manager) const;

    // Геттеры/Сеттеры
    const glm::vec3& get_position() const { return m_position; }
    void set_position(const glm::vec3& pos) { m_position = pos; }
    float get_eye_height() const { return m_eye_height; }
    bool is_on_ground() const { return m_on_ground; }
};

#endif //OPTICRAFT_PLAYER_H