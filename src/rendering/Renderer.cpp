//
// Created by noktemor on 11.02.2026.
//
#include "Renderer.h"
#include "world/Block_Types.h"
#include "utils/Logger.h"
#include <stb_image.h>
#include <iostream>
#include <algorithm>

// === Конструктор ===
Renderer::Renderer()
    : m_shader_ptr(nullptr)
    , m_vao(0)
    , m_vbo(0)
    , m_last_bound_texture(0)
{}

// === Деструктор ===
Renderer::~Renderer() {
    // Очищаем текстуры
    for (auto& [name, id] : m_textures) {
        if (id > 0) {
            glDeleteTextures(1, &id);
        }
    }

    // Очищаем кэш чанков
    for (auto& [pos, resources] : m_chunk_gl_cache) {
        cleanup_chunk_resources(resources);
    }

    // Очищаем общие буферы
    if (m_vbo > 0) glDeleteBuffers(1, &m_vbo);
    if (m_vao > 0) glDeleteVertexArrays(1, &m_vao);

    // Shader удалится автоматически через unique_ptr
}

// === Инициализация ===
bool Renderer::initialize(const std::string& vertex_shader_path,
                         const std::string& fragment_shader_path,
                         const std::string& texture_folder) {
    LOG_INFO("Initializing Renderer...");
    glEnable(GL_DEPTH_TEST);
    // 1. Шейдеры
    m_shader_ptr = std::make_unique<Shader>(vertex_shader_path.c_str(), fragment_shader_path.c_str());
    if (m_shader_ptr->get_id() == 0) {
        LOG_ERROR("Failed to compile shaders");
        return false;
    }
    LOG_INFO("Shaders compiled successfully");

    // 2. ✅ VAO (просто создаём, без VBO)
    glGenVertexArrays(1, &m_vao);
    // VBO создаём в render_chunks() для каждого чанка

    LOG_INFO("VAO created");

    // 3. Текстуры
    LOG_INFO("Loading textures from: " + texture_folder);

    for (size_t i = 0; i < static_cast<size_t>(Block_Type::Count); ++i) {
        Block_Type type = static_cast<Block_Type>(i);
        if (type == Block_Type::Air) continue;

        const auto& props = get_block_props(type);

        if (!props.texture_side.empty()) {
            load_texture(props.texture_side, texture_folder);
        }
        if (!props.texture_top.empty() && props.texture_top != props.texture_side) {
            load_texture(props.texture_top, texture_folder);
        }
        if (!props.texture_bottom.empty() && props.texture_bottom != props.texture_side) {
            load_texture(props.texture_bottom, texture_folder);
        }
    }

    LOG_INFO("Loaded " + std::to_string(m_textures.size()) + " unique textures");

    return true;
}

// === Настройка атрибутов вершин ===
void Renderer::setup_vertex_attributes() {
    // Layout Chunk_Vertex:
    // 0: position (x,y,z) - 3 floats - offset 0
    // 1: uv (u,v) - 2 floats - offset 3*sizeof(float)
    // 2: normal (nx,ny,nz) - 3 floats - offset 5*sizeof(float)

    constexpr size_t stride = sizeof(Vertex);

    // Position
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(0);

    // UV
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // Normal
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (void*)(5 * sizeof(float)));
    glEnableVertexAttribArray(2);
}

// === Загрузка текстуры из файла ===
unsigned int Renderer::load_texture(const std::string& name, const std::string& folder_path) {
    // Проверяем кэш
    auto it = m_textures.find(name);
    if (it != m_textures.end()) {
        return it->second;
    }

    // Формируем путь: folder/name.png
    std::string filepath = folder_path + "/" + name + ".png";

    int width, height, channels;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load(filepath.c_str(), &width, &height, &channels, 4);

    if (!data) {
        LOG_ERROR("Failed to load textures: " + filepath);
        unsigned int placeholder = create_placeholder_texture();
        m_textures[name] = placeholder;
        return placeholder;
    }

    unsigned int texture_id;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    // Параметры для пиксель-арта (без размытия)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Загрузка данных
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    stbi_image_free(data);

    m_textures[name] = texture_id;
    LOG_DEBUG("Loaded textures: " + name + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");

    return texture_id;
}

// === Заглушка-текстура (пурпурный квадрат) ===
unsigned int Renderer::create_placeholder_texture() {
    unsigned int id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);

    uint32_t placeholder[16 * 16];
    for (int i = 0; i < 16 * 16; ++i) {
        placeholder[i] = 0xFFFF00FF; // Пурпурный: R=255, G=0, B=255, A=255
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, placeholder);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    return id;
}

// === Получить текстуру для грани блока ===
unsigned int Renderer::get_texture_for_face(Block_Type type, int face_direction) const {
    const auto& props = get_block_props(type);

    // Верх (+Y)
    if (face_direction == 1 && !props.texture_top.empty()) {
        auto it = m_textures.find(props.texture_top);
        if (it != m_textures.end()) return it->second;
    }

    // Низ (-Y)
    if (face_direction == -1 && !props.texture_bottom.empty()) {
        auto it = m_textures.find(props.texture_bottom);
        if (it != m_textures.end()) return it->second;
    }

    // Бока / дефолт
    if (!props.texture_side.empty()) {
        auto it = m_textures.find(props.texture_side);
        if (it != m_textures.end()) return it->second;
    }

    // Фолбэк: любая доступная текстура блока
    if (!props.texture_top.empty()) {
        auto it = m_textures.find(props.texture_top);
        if (it != m_textures.end()) return it->second;
    }

    // Совсем фолбэк: заглушка
    return 0;
}

// === Умный бинд текстуры (только если сменилась) ===
void Renderer::bind_texture(unsigned int texture_id) {
    if (texture_id > 0 && m_last_bound_texture != texture_id) {
        glBindTexture(GL_TEXTURE_2D, texture_id);
        m_last_bound_texture = texture_id;
    }
}

// === Рендер группы вершин ===
void Renderer::render_vertex_batch(const std::vector<Vertex>& vertices, bool is_transparent) {
    if (vertices.empty()) return;

    // Обновляем VBO
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                vertices.size() * sizeof(Vertex),
                vertices.data(),
                GL_DYNAMIC_DRAW);

    // Настраиваем blending для прозрачных
    if (is_transparent) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);  // Не пишем в depth buffer
    }

    // Рендерим
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));

    // Сбрасываем blending
    if (is_transparent) {
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
}

// === Рендер одного чанка (для отладки) ===
void Renderer::render_chunk(const std::vector<Vertex>& vertices,
                           const glm::mat4& view,
                           const glm::mat4& projection) {
    if (vertices.empty()) return;

    m_shader_ptr->use();
    m_shader_ptr->set_mat4("view", view);
    m_shader_ptr->set_mat4("projection", projection);
    m_shader_ptr->set_int("texture0", 0);
    glm::mat4 model = glm::mat4(1.0f);  // Identity matrix
    m_shader_ptr->set_mat4("model", model);

    // Группируем вершины по прозрачности
    std::vector<Vertex> opaque, transparent;
    for (const auto& v : vertices) {
        // Простая эвристика: если нормаль имеет компонент Y и блок — листва/вода
        // В идеале: хранить block_type в Vertex
        if (v.nx == 0 && v.ny == 0 && v.nz == 0) {
            transparent.push_back(v);  // заглушка
        } else {
            opaque.push_back(v);
        }
    }

    glBindVertexArray(m_vao);

    // 1️ Непрозрачные
    if (!opaque.empty()) {
        // Для простоты: биндим первую доступную текстуру
        if (!m_textures.empty()) {
            bind_texture(m_textures.begin()->second);
            m_shader_ptr->set_int("texture0", 0);
        }
        render_vertex_batch(opaque, false);
    }

    // 2️Прозрачные
    if (!transparent.empty()) {
        if (!m_textures.empty()) {
            bind_texture(m_textures.begin()->second);
            m_shader_ptr->set_int("texture0", 0);
        }
        render_vertex_batch(transparent, true);
    }

    glBindVertexArray(0);
}

// === Рендер множества чанков (основной метод) ===
void Renderer::render_chunks(const std::vector<std::pair<glm::ivec2, std::vector<Vertex>>>& chunks,
                            const glm::mat4& view,
                            const glm::mat4& projection) {
    if (chunks.empty()) {
        LOG_WARN("No chunks to render!");
        return;
    }

    m_shader_ptr->use();
    m_shader_ptr->set_mat4("view", view);
    m_shader_ptr->set_mat4("projection", projection);
    m_shader_ptr->set_int("texture0", 0);

    glActiveTexture(GL_TEXTURE0);

    // 🔹 Биндим первую доступную текстуру (для всех чанков)
    unsigned int default_tex = 0;
    if (!m_textures.empty()) {
        default_tex = m_textures.begin()->second;
    }
    glBindTexture(GL_TEXTURE_2D, default_tex);

    size_t total_drawn = 0;

    // 🔄 Для каждого чанка:
    for (const auto& [chunk_pos, vertices] : chunks) {
        if (vertices.empty()) {
            std::cout << "[SKIP] Chunk (" << chunk_pos.x << "," << chunk_pos.y
                      << ") has 0 vertices\n";
            continue;
        }

        // === 🔥 КЛЮЧЕВОЙ МОМЕНТ: VBO внутри цикла ===
        unsigned int vbo;
        glGenBuffers(1, &vbo);

        // 1. Биндим VAO
        glBindVertexArray(m_vao);

        // 2. Биндим VBO к VAO
        glBindBuffer(GL_ARRAY_BUFFER, vbo);

        // 3. ✅ ЗАГРУЖАЕМ ДАННЫЕ В VBO
        glBufferData(GL_ARRAY_BUFFER,
                    vertices.size() * sizeof(Vertex),
                    vertices.data(),
                    GL_STATIC_DRAW);

        // 4. ✅ НАСТРАИВАЕМ АТРИБУТЫ (должно быть ПОСЛЕ glBindBuffer!)
        // Position (location = 0)
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
        glEnableVertexAttribArray(0);

        // UV (location = 1)
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        // Normal (location = 2)
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(5 * sizeof(float)));
        glEnableVertexAttribArray(2);

        // 5. Рендерим
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
        total_drawn += vertices.size();

        // 6. ✅ УДАЛЯЕМ VBO (можно кэшировать для оптимизации)
        glDeleteBuffers(1, &vbo);

        /*std::cout << "[RENDER] Chunk (" << chunk_pos.x << "," << chunk_pos.z
                  << "): " << vertices.size() << " vertices\n";*/
    }

    std::cout << "[RENDER] Total: " << total_drawn << " vertices drawn\n";

    glBindVertexArray(0);
}

// === Очистка кэша чанка ===
void Renderer::invalidate_chunk_cache(const glm::ivec2& chunk_pos) {
    auto it = m_chunk_gl_cache.find(chunk_pos);
    if (it != m_chunk_gl_cache.end()) {
        cleanup_chunk_resources(it->second);
        m_chunk_gl_cache.erase(it);
    }
}

void Renderer::cleanup_chunk_resources(const Chunk_GL_Resources& resources) {
    if (resources.vbo_id > 0) {
        glDeleteBuffers(1, const_cast<unsigned int*>(&resources.vbo_id));
    }
}

// === Очистка экрана ===
void Renderer::clear_screen(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}