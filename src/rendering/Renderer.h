//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_RENDERER_H
#define OPTICRAFT_RENDERER_H

#include <glad/gl.h>
#include <vector>
#include <memory>
#include <unordered_map>
#include <glm/glm.hpp>
#include "Shader.h"
#include "world/Chunk.h"
#include "utils/Hash_Utils.h"
#include "Texture_Atlas.h"

// Используем Chunk_Vertex из Chunk.h
using Vertex = Chunk_Vertex;


class Renderer {
private:
    Shader* m_shader_ptr;
    unsigned int m_vao;
    unsigned int m_vbo;  // Общий VAO/VBO для формата


    struct Chunk_GL_Resources {
        unsigned int vbo_id = 0;
        size_t vertex_count = 0;
        bool is_valid = false;
    };

    std::unordered_map<glm::ivec2, Chunk_GL_Resources, Chunk_Key_Hash> m_chunk_gl_cache;

    void setup_buffers();

    Texture_Atlas m_texture_atlas;

public:
    Renderer();
    ~Renderer();

    bool initialize(const std::string& vertex_shader_path,
                         const std::string& fragment_shader_path,
                         const std::string& texture_atlas_pat,
                         int tiles_per_row = 64,
                         int tiles_per_column = 32);

    void clear_screen(float r, float g, float b, float a = 1.0f);

    void render_chunk(const std::vector<Vertex>& vertices,
                     const glm::mat4& view,
                     const glm::mat4& projection);

    void render_chunks(const std::vector<std::pair<glm::ivec2, std::vector<Vertex>>>& chunks,
                      const glm::mat4& view,
                      const glm::mat4& projection);

    Shader* get_shader() { return m_shader_ptr; }

    // ✅ Очистка кэша при выгрузке чанка
    void invalidate_chunk_cache(const glm::ivec2& chunk_pos);
};
#endif // OPTICRAFT_RENDERER_H