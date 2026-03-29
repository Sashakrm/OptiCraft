//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_CHUNK_H
#define OPTICRAFT_CHUNK_H

#include <array>
#include <vector>
#include <glm/glm.hpp>
#include "Block_Types.h"
#include "utils/Config.h"

struct Chunk_Vertex {
    float x, y, z;
    float u, v;
    float nx, ny, nz;
};

class Chunk {
private:
    int m_chunk_x;
    int m_chunk_z;

    // 3D массив блоков: [y][z][x]
    std::array<std::array<std::array<Block_Type, Config::chunk_size>, Config::chunk_size>, Config::chunk_height> m_blocks;

    // Меш
    std::vector<Chunk_Vertex> m_mesh_vertices;
    bool m_is_mesh_dirty;

    // OpenGL
    unsigned int m_vao_id;
    unsigned int m_vbo_id;

    void build_mesh_internal();
    bool is_face_visible(int x, int y, int z, int dx, int dy, int dz) const;
    void add_face(int x, int y, int z, int dx, int dy, int dz, Block_Type block_type);

public:
    Chunk(int chunk_x, int chunk_z);

    // Генерация
    void generate_blocks();
    void build_mesh();

    // Доступ к блокам
    Block_Type get_block(int x, int y, int z) const;
    void set_block(int x, int y, int z, Block_Type type);

    // Геттеры
    int get_chunk_x() const { return m_chunk_x; }
    int get_chunk_z() const { return m_chunk_z; }
    const std::vector<Chunk_Vertex>& get_vertices() const { return m_mesh_vertices; }
    bool is_mesh_dirty() const { return m_is_mesh_dirty; }
    bool is_empty() const { return m_mesh_vertices.empty(); }

    // Позиция в мире
    glm::vec3 get_world_position(int local_x, int local_y, int local_z) const;
};

#endif // OPTICRAFT_CHUNK_H