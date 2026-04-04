//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_RENDERER_H
#define OPTICRAFT_RENDERER_H

#include <glad/gl.h>
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <glm/glm.hpp>

#include "Shader.h"
#include "world/Chunk.h"
#include "world/Block_Types.h"
#include "utils/Hash_Utils.h"

// ✅ Алиас для удобства
using Vertex = Chunk_Vertex;


class Renderer {
private:
    // === Шейдеры ===
    std::unique_ptr<Shader> m_shader_ptr;

    // === OpenGL буферы (общие) ===
    unsigned int m_vao;
    unsigned int m_vbo;

    // ===  Менеджер текстур ===
    std::unordered_map<std::string, unsigned int> m_textures;  // имя → GL ID
    unsigned int m_last_bound_texture;

    // === Кэш ресурсов чанков (VBO) ===
    struct Chunk_GL_Resources {
        unsigned int vbo_id = 0;
        size_t vertex_count = 0;
        bool is_valid = false;
    };
    std::unordered_map<glm::ivec2, Chunk_GL_Resources, Chunk_Key_Hash> m_chunk_gl_cache;

    // === Вспомогательные методы ===
    void setup_vertex_attributes();
    unsigned int load_texture(const std::string& name, const std::string& folder_path);
    unsigned int create_placeholder_texture();
    unsigned int get_texture_for_face(Block_Type type, int face_direction) const;
    void bind_texture(unsigned int texture_id);

    // === Рендер групп вершин ===
    void render_vertex_batch(const std::vector<Vertex>& vertices, bool is_transparent);

    // === Очистка ===
    void cleanup_chunk_resources(const Chunk_GL_Resources& resources);

public:
    Renderer();
    ~Renderer();

    //  Инициализация: папка с текстурами вместо атласа
    bool initialize(const std::string& vertex_shader_path,
                   const std::string& fragment_shader_path,
                   const std::string& texture_folder);  // ← "assets/textures"

    // Очистка экрана
    void clear_screen(float r, float g, float b, float a = 1.0f);

    // Рендер одного чанка (для отладки)
    void render_chunk(const std::vector<Vertex>& vertices,
                     const glm::mat4& view,
                     const glm::mat4& projection);

    // Рендер множества чанков (основной метод)
    void render_chunks(const std::vector<std::pair<glm::ivec2, std::vector<Vertex>>>& chunks,
                      const glm::mat4& view,
                      const glm::mat4& projection);

    // Доступ к шейдеру (для установки униформов извне)
    Shader* get_shader() { return m_shader_ptr.get(); }

    // Очистка кэша при выгрузке чанка
    void invalidate_chunk_cache(const glm::ivec2& chunk_pos);

    // Статистика (для отладки)
    size_t get_loaded_texture_count() const { return m_textures.size(); }
};

#endif // OPTICRAFT_RENDERER_H