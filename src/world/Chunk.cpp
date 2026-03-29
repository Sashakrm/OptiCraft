//
// Created by noktemor on 11.02.2026.
//

#include "Chunk.h"
#include "Block_Types.h"
#include "utils/Math_Helpers.h"
#include <cmath>
#include <glad/gl.h>
#include <iostream>

Chunk::Chunk(int chunk_x, int chunk_z)
    : m_chunk_x(chunk_x)
    , m_chunk_z(chunk_z)
    , m_is_mesh_dirty(true)
    , m_vao_id(0)
    , m_vbo_id(0)
{
    // Инициализируем все блоки как Air
    for (int y = 0; y < Config::chunk_height; ++y) {
        for (int z = 0; z < Config::chunk_size; ++z) {
            for (int x = 0; x < Config::chunk_size; ++x) {
                m_blocks[y][z][x] = Block_Type::Air;
            }
        }
    }
}

void Chunk::generate_blocks() {
    // Простая генерация: несколько слоёв
    for (int y = 0; y < Config::chunk_height; ++y) {
        for (int z = 0; z < Config::chunk_size; ++z) {
            for (int x = 0; x < Config::chunk_size; ++x) {
                if (y == 0) {
                    m_blocks[y][z][x] = Block_Type::Stone;
                } else if (y < 4) {
                    m_blocks[y][z][x] = Block_Type::Dirt;
                } else if (y == 4) {
                    m_blocks[y][z][x] = Block_Type::Grass;
                } else {
                    m_blocks[y][z][x] = Block_Type::Air;
                }
            }
        }
    }
    m_is_mesh_dirty = true;
}

void Chunk::build_mesh() {
    m_mesh_vertices.clear();

    for (int y = 0; y < Config::chunk_height; ++y) {
        for (int z = 0; z < Config::chunk_size; ++z) {
            for (int x = 0; x < Config::chunk_size; ++x) {
                Block_Type block = m_blocks[y][z][x];
                if (block == Block_Type::Air) continue;

                // Проверяем 6 граней
                if (is_face_visible(x, y, z, 1, 0, 0))  // +X
                    add_face(x, y, z, 1, 0, 0, block);
                if (is_face_visible(x, y, z, -1, 0, 0)) // -X
                    add_face(x, y, z, -1, 0, 0, block);
                if (is_face_visible(x, y, z, 0, 1, 0))  // +Y
                    add_face(x, y, z, 0, 1, 0, block);
                if (is_face_visible(x, y, z, 0, -1, 0)) // -Y
                    add_face(x, y, z, 0, -1, 0, block);
                if (is_face_visible(x, y, z, 0, 0, 1))  // +Z
                    add_face(x, y, z, 0, 0, 1, block);
                if (is_face_visible(x, y, z, 0, 0, -1)) // -Z
                    add_face(x, y, z, 0, 0, -1, block);
            }
        }
    }

    if (m_vbo_id == 0) {
        glGenBuffers(1, &m_vbo_id);
    }

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo_id);
    glBufferData(GL_ARRAY_BUFFER,
                 m_mesh_vertices.size() * sizeof(Chunk_Vertex),
                 m_mesh_vertices.data(),
                 GL_STATIC_DRAW);  // STATIC = данные меняются редко


    m_is_mesh_dirty = false;
}

bool Chunk::is_face_visible(int x, int y, int z, int dx, int dy, int dz) const {
    int nx = x + dx;
    int ny = y + dy;
    int nz = z + dz;

    // Если сосед за пределами чанка — грань видима (упрощённо)
    if (nx < 0 || nx >= Config::chunk_size ||
        ny < 0 || ny >= Config::chunk_height ||
        nz < 0 || nz >= Config::chunk_size) {
        return true;
    }

    // Если сосед — воздух, грань видима
    return m_blocks[ny][nz][nx] == Block_Type::Air;
}

void Chunk::add_face(int x, int y, int z, int dx, int dy, int dz, Block_Type block_type) {
    // === 1. Координаты в мире ===
    float wx = static_cast<float>(m_chunk_x * Config::chunk_size + x);
    float wy = static_cast<float>(y);
    float wz = static_cast<float>(m_chunk_z * Config::chunk_size + z);

    // === 2. Нормаль ===
    float nx = static_cast<float>(dx);
    float ny = static_cast<float>(dy);
    float nz = static_cast<float>(dz);

    // === 3. Текстура: получаем свойства блока и выбираем тайл ===
    const Block_Properties& props = get_block_props(block_type);

    // Определяем индекс тайла в зависимости от направления грани
    int texture_index;
    if (dy == 1) {
        texture_index = props.texture_index_top;      // +Y (верх)
    } else if (dy == -1) {
        texture_index = props.texture_index_bottom;   // -Y (низ)
    } else {
        texture_index = props.texture_index_side;     // боковые грани
    }

    // Если индекс -1 (воздух/невалид) — пропускаем
    if (texture_index < 0) return;

    // === 4. UV-координаты из атласа ===
    constexpr int TILES_PER_ROW = 64;
    constexpr int TILES_PER_COLUMN = 32;

    // Вычисляем координаты тайла в сетке атласа
    int tile_row = texture_index / TILES_PER_ROW;
    int tile_col = texture_index % TILES_PER_ROW;

    // UV в диапазоне [0, 1] для всего атласа
    float u_min = static_cast<float>(tile_col) / static_cast<float>(TILES_PER_ROW);
    float v_min = 1.0f - static_cast<float>(tile_row + 1) / static_cast<float>(TILES_PER_COLUMN);  // OpenGL V идёт снизу-вверх
    float u_max = u_min + 1.0f / static_cast<float>(TILES_PER_ROW);
    float v_max = v_min + 1.0f / static_cast<float>(TILES_PER_COLUMN);

    // === 5. Вершины: порядок ПО ЧАСОВОЙ стрелке (CW) при взгляде СНАРУЖИ ===
    // Формат: {x, y, z, u, v, nx, ny, nz}

    if (dy == 1) { // ===== ВЕРХ (+Y) =====
        // Треугольник 1
        m_mesh_vertices.push_back({wx,     wy + 1, wz,     u_min, v_max, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy + 1, wz,     u_max, v_max, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy + 1, wz + 1, u_max, v_min, nx, ny, nz});
        // Треугольник 2
        m_mesh_vertices.push_back({wx,     wy + 1, wz,     u_min, v_max, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy + 1, wz + 1, u_max, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx,     wy + 1, wz + 1, u_min, v_min, nx, ny, nz});

    } else if (dy == -1) { // ===== НИЗ (-Y) =====
        m_mesh_vertices.push_back({wx,     wy, wz,     u_min, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx,     wy, wz + 1, u_min, v_max, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy, wz + 1, u_max, v_max, nx, ny, nz});

        m_mesh_vertices.push_back({wx,     wy, wz,     u_min, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy, wz + 1, u_max, v_max, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy, wz,     u_max, v_min, nx, ny, nz});

    } else if (dx == 1) { // ===== +X =====
        m_mesh_vertices.push_back({wx + 1, wy,     wz,     u_min, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy,     wz + 1, u_min, v_max, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy + 1, wz + 1, u_max, v_max, nx, ny, nz});

        m_mesh_vertices.push_back({wx + 1, wy,     wz,     u_min, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy + 1, wz + 1, u_max, v_max, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy + 1, wz,     u_max, v_min, nx, ny, nz});

    } else if (dx == -1) { // ===== -X =====
        m_mesh_vertices.push_back({wx,     wy,     wz + 1, u_min, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx,     wy,     wz,     u_max, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx,     wy + 1, wz,     u_max, v_max, nx, ny, nz});

        m_mesh_vertices.push_back({wx,     wy,     wz + 1, u_min, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx,     wy + 1, wz,     u_max, v_max, nx, ny, nz});
        m_mesh_vertices.push_back({wx,     wy + 1, wz + 1, u_min, v_max, nx, ny, nz});

    } else if (dz == 1) { // ===== +Z =====
        m_mesh_vertices.push_back({wx,     wy,     wz + 1, u_min, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy,     wz + 1, u_max, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy + 1, wz + 1, u_max, v_max, nx, ny, nz});

        m_mesh_vertices.push_back({wx,     wy,     wz + 1, u_min, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy + 1, wz + 1, u_max, v_max, nx, ny, nz});
        m_mesh_vertices.push_back({wx,     wy + 1, wz + 1, u_min, v_max, nx, ny, nz});

    } else if (dz == -1) { // ===== -Z =====
        m_mesh_vertices.push_back({wx + 1, wy,     wz,     u_min, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx,     wy,     wz,     u_max, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx,     wy + 1, wz,     u_max, v_max, nx, ny, nz});

        m_mesh_vertices.push_back({wx + 1, wy,     wz,     u_min, v_min, nx, ny, nz});
        m_mesh_vertices.push_back({wx,     wy + 1, wz,     u_max, v_max, nx, ny, nz});
        m_mesh_vertices.push_back({wx + 1, wy + 1, wz,     u_max, v_min, nx, ny, nz});
    }
}

Block_Type Chunk::get_block(int x, int y, int z) const {
    if (x < 0 || x >= Config::chunk_size ||
        y < 0 || y >= Config::chunk_height ||
        z < 0 || z >= Config::chunk_size) {
        return Block_Type::Air;
    }
    return m_blocks[y][z][x];
}

void Chunk::set_block(int x, int y, int z, Block_Type type) {
    if (x < 0 || x >= Config::chunk_size ||
        y < 0 || y >= Config::chunk_height ||
        z < 0 || z >= Config::chunk_size) {
        return;
    }
    m_blocks[y][z][x] = type;
    m_is_mesh_dirty = true;
}

glm::vec3 Chunk::get_world_position(int local_x, int local_y, int local_z) const {
    return glm::vec3(
        static_cast<float>(m_chunk_x * Config::chunk_size + local_x),
        static_cast<float>(local_y),
        static_cast<float>(m_chunk_z * Config::chunk_size + local_z)
    );
}