//
// Created by noktemor on 11.02.2026.
//

#include "Player.h"
#include "utils/Config.h"
#include "utils/Math_Helpers.h"
#include <cmath>
#include <iostream>

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

void Player::update(float delta_time, const Chunk_Manager& chunk_manager, const Camera* camera) {
    // 1. Гравитация
    m_velocity.y += m_gravity * delta_time;

    // 2. Движение относительно камеры
    if (camera && (m_pending_input.forward != 0.0f || m_pending_input.strafe != 0.0f)) {
        glm::vec3 camera_front = camera->get_front();
        glm::vec3 camera_right = camera->get_right();

        // Обнуляем Y для движения (чтобы не летать вверх/вниз)
        camera_front.y = 0.0f;
        camera_right.y = 0.0f;

        camera_front = glm::normalize(camera_front);
        camera_right = glm::normalize(camera_right);

        glm::vec3 move_dir = camera_front * m_pending_input.forward +
                            camera_right * m_pending_input.strafe;

        if (glm::length(move_dir) > 0.001f) {
            move_dir = glm::normalize(move_dir);
            m_velocity.x = move_dir.x * m_move_speed;
            m_velocity.z = move_dir.z * m_move_speed;
        }
    } else {
        // ✅ Останавливаемся, если ввод не нажат
        m_velocity.x = 0.0f;
        m_velocity.z = 0.0f;
    }

    // 3. Прыжок
    if (m_pending_input.jump && m_on_ground) {
        m_velocity.y = m_jump_force;
        m_on_ground = false;
    }

    // 4. === 🔥 НОВАЯ КОЛЛИЗИЯ ВМЕСТО "y < 5" ===
    glm::vec3 new_pos = m_position + m_velocity * delta_time;
    m_position = resolve_collision(new_pos, chunk_manager);


    // 5. ✅ СБРОС ВВОДА (важно!)
    m_pending_input = {0.0f, 0.0f, false};

    static int debug_frame = 0;
    if (++debug_frame % 60 == 0) {
        glm::vec3 feet = m_position;  // Низ игрока
        Block_Type below = get_block_at(feet, chunk_manager);
        const auto& props = get_block_props(below);

        std::cout << "[COLLISION] Player Y=" << static_cast<int>(m_position.y)
                  << " | Block below: " << static_cast<int>(below)
                  << " (solid=" << props.is_solid << ")"
                  << " | velocity.y=" << m_velocity.y
                  << " | on_ground=" << m_on_ground << "\n";
    }
}

void Player::set_movement_input(const Input_Manager::Move_Input& input) {
    m_pending_input = input;
}


Block_Type Player::get_block_at(const glm::vec3& world_pos, const Chunk_Manager& chunk_manager) const {
    // 1. Координаты чанка
    int cx = static_cast<int>(std::floor(world_pos.x / Config::chunk_size));
    int cz = static_cast<int>(std::floor(world_pos.z / Config::chunk_size));

    // 2. Локальные координаты ВНУТРИ чанка
    int lx = static_cast<int>(std::floor(world_pos.x)) % Config::chunk_size;
    int lz = static_cast<int>(std::floor(world_pos.z)) % Config::chunk_size;

    // ✅ КЛЮЧЕВОЕ: конверсия мирового Y в локальный Y чанка
    // Чанк покрывает [world_min_y, world_min_y + chunk_height)
    int world_y = static_cast<int>(std::floor(world_pos.y));
    int local_y = world_y - Config::world_min_y;  // ✅ Сдвиг на минимум мира

    // 3. Обработка отрицательных координат
    if (lx < 0) lx += Config::chunk_size;
    if (lz < 0) lz += Config::chunk_size;

    // 4. Проверка границ локального Y
    if (local_y < 0 || local_y >= Config::chunk_height) {
        return Block_Type::Air;  // За пределами чанка = воздух
    }

    // 5. Получаем чанк и блок
    const Chunk* chunk = chunk_manager.get_chunk(cx, cz);
    if (chunk) {
        return chunk->get_block(lx, local_y, lz);  // ✅ local_y вместо world_y
    }

    return Block_Type::Air;
}

bool Player::check_block_collision(const glm::vec3& pos, const Chunk_Manager& chunk_manager) const {
    // AABB игрока (половина ширины с каждой стороны)
    float half_width = m_size.x * 0.5f;
    float height = m_size.y;

    // Проверяем блоки в bounding box игрока
    int min_x = static_cast<int>(std::floor(pos.x - half_width));
    int max_x = static_cast<int>(std::floor(pos.x + half_width));
    int min_y = static_cast<int>(std::floor(pos.y));
    int max_y = static_cast<int>(std::floor(pos.y + height));
    int min_z = static_cast<int>(std::floor(pos.z - half_width));
    int max_z = static_cast<int>(std::floor(pos.z + half_width));

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            for (int z = min_z; z <= max_z; ++z) {
                Block_Type block = get_block_at(glm::vec3(x, y, z), chunk_manager);

                // ✅ Твёрдые блоки = коллизия
                if (block != Block_Type::Air && block != Block_Type::Water) {
                    return true;
                }
            }
        }
    }

    return false;
}

glm::vec3 Player::resolve_collision(const glm::vec3& new_pos, const Chunk_Manager& chunk_manager) {
    glm::vec3 result = new_pos;

    // 🔹 1. Проверка земли: блок ПОД ногами (не на уровне ног!)
    // Ноги игрока: new_pos.y
    // Блок под ногами: new_pos.y - 0.5 (половина блока)
    glm::vec3 feet_check_pos = glm::vec3(
        new_pos.x,
        new_pos.y - 0.5f,  // ✅ Проверяем на пол-блока ниже позиции игрока
        new_pos.z
    );

    Block_Type block_below = get_block_at(feet_check_pos, chunk_manager);
    const auto& props = get_block_props(block_below);

    // ✅ Если блок твёрдый И игрок падал вниз — останавливаем
    if (props.is_solid && m_velocity.y <= 0.0f) {
        // Ставим игрока ПОВЕРХ блока
        result.y = std::floor(feet_check_pos.y) + 1.0f + 0.001f;  // +0.001 чтобы не залипал
        m_velocity.y = 0.0f;
        m_on_ground = true;
    } else {
        m_on_ground = false;
    }

    // 🔹 2. Горизонтальная коллизия (упрощённо)
    // Проверяем блоки на уровне середины тела
    const float check_radius = m_size.x * 0.4f;
    const float check_height = m_size.y * 0.5f;  // Середина роста

    for (int dx : {-1, 0, 1}) {
        for (int dz : {-1, 0, 1}) {
            if (dx == 0 && dz == 0) continue;

            glm::vec3 side_check(
                new_pos.x + dx * check_radius,
                new_pos.y + check_height,
                new_pos.z + dz * check_radius
            );

            Block_Type side_block = get_block_at(side_check, chunk_manager);
            if (get_block_props(side_block).is_solid) {
                // Отменяем движение в направлении коллизии
                if (dx != 0) result.x = m_position.x;
                if (dz != 0) result.z = m_position.z;
            }
        }
    }

    return result;
}