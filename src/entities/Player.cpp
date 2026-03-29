//
// Created by noktemor on 11.02.2026.
//

#include "Player.h"
#include "utils/Config.h"
#include "utils/Math_Helpers.h"
#include <cmath>

Player::Player()
    : m_position(0.0f, 10.0f, 0.0f)
    , m_velocity(0.0f)
    , m_size(Config::player_width, Config::player_height, Config::player_width)
    , m_on_ground(false)
    , m_eye_height(Config::player_eye_height)
    , m_move_speed(Config::player_move_speed)
    , m_jump_force(Config::player_jump_force)
    , m_gravity(Config::player_gravity)
{}

void Player::update(float delta_time, const Chunk_Manager& chunk_manager) {
    // Применяем гравитацию
    m_velocity.y += m_gravity * delta_time;

    // Применяем ввод движения
    if (m_pending_input.forward != 0.0f || m_pending_input.strafe != 0.0f) {
        // Нормализуем вектор движения
        float length = std::sqrt(m_pending_input.forward * m_pending_input.forward +
                                 m_pending_input.strafe * m_pending_input.strafe);
        if (length > 0.001f) {
            m_velocity.x = (m_pending_input.strafe / length) * m_move_speed;
            m_velocity.z = -(m_pending_input.forward / length) * m_move_speed;
        }
    } else {
        m_velocity.x = 0.0f;
        m_velocity.z = 0.0f;
    }

    // Прыжок
    if (m_pending_input.jump && m_on_ground) {
        m_velocity.y = m_jump_force;
        m_on_ground = false;
    }

    // Применяем скорость к позиции (с простой коллизией)
    glm::vec3 new_pos = m_position + m_velocity * delta_time;

    // Простая проверка коллизии с землёй
    if (new_pos.y < 5.0f) { // Упрощённо: земля на Y=5
        new_pos.y = 5.0f;
        m_velocity.y = 0.0f;
        m_on_ground = true;
    } else {
        m_on_ground = false;
    }

    m_position = new_pos;

    // Сбрасываем ввод
    m_pending_input = {0.0f, 0.0f, false};
}

void Player::set_movement_input(const Input_Manager::Move_Input& input) {
    m_pending_input = input;
}

bool Player::check_collision(const glm::vec3& new_pos, const Chunk_Manager& chunk_manager) const {
    // Упрощённая проверка — можно улучшить
    return new_pos.y < 0.0f;
}

Block_Type Player::get_block_at(const glm::vec3& world_pos, const Chunk_Manager& chunk_manager) const {
    int cx = static_cast<int>(std::floor(world_pos.x / Config::chunk_size));
    int cz = static_cast<int>(std::floor(world_pos.z / Config::chunk_size));

    int lx = static_cast<int>(world_pos.x) % Config::chunk_size;
    int ly = static_cast<int>(world_pos.y);
    int lz = static_cast<int>(world_pos.z) % Config::chunk_size;

    if (lx < 0) lx += Config::chunk_size;
    if (lz < 0) lz += Config::chunk_size;

    const Chunk* chunk = chunk_manager.get_chunk(cx, cz);
    if (chunk) {
        return chunk->get_block(lx, ly, lz);
    }

    return Block_Type::Air;
}