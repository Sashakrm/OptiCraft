//
// Created by noktemor on 11.02.2026.
//
#include "Renderer.h"
#include "utils/Logger.h"

Renderer::Renderer()
    : m_shader_ptr(nullptr)
    , m_vao(0)
    , m_vbo(0)
{}

Renderer::~Renderer() {
    if (m_vao != 0) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo != 0) glDeleteBuffers(1, &m_vbo);

    // ✅ Очищаем кэш VBO
    for (auto& [key, res] : m_chunk_gl_cache) {
        if (res.vbo_id != 0) {
            glDeleteBuffers(1, &res.vbo_id);
        }
    }

    delete m_shader_ptr;
}

bool Renderer::initialize(const std::string& vertex_shader_path,
                         const std::string& fragment_shader_path,
                         const std::string& texture_atlas_path,
                         int tiles_per_row,
                         int tiles_per_column) {
    m_shader_ptr = new Shader(vertex_shader_path, fragment_shader_path);
    if (m_shader_ptr->get_id() == 0) {
        LOG_ERROR("Failed to create shader program");
        return false;
    }

    //  Загружаем атлас
    if (!m_texture_atlas.load_from_file(texture_atlas_path, tiles_per_row, tiles_per_column)) {
        LOG_ERROR("Failed to load texture atlas");
        return false;
    }

    setup_buffers();
    LOG_INFO("Renderer initialized");
    return true;
}

void Renderer::setup_buffers() {
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    // Vertex layout: pos(3) + uv(2) + normal(3) = 8 floats
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

void Renderer::clear_screen(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
}

void Renderer::render_chunk(const std::vector<Vertex>& vertices,
                           const glm::mat4& view,
                           const glm::mat4& projection) {
    if (vertices.empty()) return;

    m_shader_ptr->use();
    m_shader_ptr->set_mat4("model", glm::mat4(1.0f));
    m_shader_ptr->set_mat4("view", view);
    m_shader_ptr->set_mat4("projection", projection);

    //  Привязываем текстуру к юниту 0
    m_texture_atlas.bind(0);

    //  Сообщаем шейдеру, что текстура в юните 0
    m_shader_ptr->set_int("textureAtlas", 0);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);

    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));

    glBindVertexArray(0);
}

void Renderer::render_chunks(const std::vector<std::pair<glm::ivec2, std::vector<Vertex>>>& chunks,
                            const glm::mat4& view,
                            const glm::mat4& projection) {
    m_shader_ptr->use();
    m_shader_ptr->set_mat4("model", glm::mat4(1.0f));
    m_shader_ptr->set_mat4("view", view);
    m_shader_ptr->set_mat4("projection", projection);
    m_shader_ptr->set_int("textureAtlas", 0);  // ✅ Юнит 0
    //  Не биндим VAO здесь — будем биндить внутри цикла с правильным буфером

    m_texture_atlas.bind(0);

    m_shader_ptr->set_int("textureAtlas", 0);




    for (const auto& [chunk_pos, vertices] : chunks) {
        if (vertices.empty()) continue;

        auto& gl_res = m_chunk_gl_cache[chunk_pos];

        // Загружаем данные, если нужно
        if (!gl_res.is_valid || gl_res.vertex_count != vertices.size()) {
            if (gl_res.vbo_id == 0) {
                glGenBuffers(1, &gl_res.vbo_id);
            }

            glBindBuffer(GL_ARRAY_BUFFER, gl_res.vbo_id);
            glBufferData(GL_ARRAY_BUFFER,
                        vertices.size() * sizeof(Vertex),
                        vertices.data(),
                        GL_STATIC_DRAW);

            gl_res.vertex_count = vertices.size();
            gl_res.is_valid = true;
        }

        //  КЛЮЧЕВОЙ ФИКС: Биндим VAO + VBO + ПЕРЕЗАПИСЫВАЕМ АТРИБУТЫ
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, gl_res.vbo_id);

        // Явно указываем, что атрибуты берутся из ТЕКУЩЕГО буфера
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(5 * sizeof(float)));
        glEnableVertexAttribArray(2);

        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(gl_res.vertex_count));
    }

    glBindVertexArray(0);
}

void Renderer::invalidate_chunk_cache(const glm::ivec2& chunk_pos) {
    auto it = m_chunk_gl_cache.find(chunk_pos);
    if (it != m_chunk_gl_cache.end()) {
        if (it->second.vbo_id != 0) {
            glDeleteBuffers(1, &it->second.vbo_id);
        }
        m_chunk_gl_cache.erase(it);
    }
}