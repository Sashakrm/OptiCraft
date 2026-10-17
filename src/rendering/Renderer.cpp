//
// Created by noktemor on 11.02.2026.
//
#include <cstddef>
#include <cstdlib>
#include "rendering/Renderer.h"
#include "utils/logging/Logger.h"
#include "utils/Config.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>

void Renderer::begin_sub_viewport(int x, int y_top, int width, int height, int window_height) {
    const int y = window_height - (y_top + height);   // GL отсчитывает снизу
    glViewport(x, y, width, height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, width, height);
    glClear(GL_DEPTH_BUFFER_BIT);   // превью рисуется поверх уже отрисованного HUD без чужой глубины
    glEnable(GL_DEPTH_TEST);
}

void Renderer::end_sub_viewport(int window_width, int window_height) {
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, window_width, window_height);
}

namespace {
    // Компиляция и линковка compute-шейдера (GL 4.3). 0 при ошибке, причина — в логе.
    unsigned int compile_compute_program(const std::string& path) {
        std::ifstream file(path);
        if (!file) {
            LOG_ERROR("Cannot open compute shader: " + path);
            return 0;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::string source = buffer.str();
        const char* source_ptr = source.c_str();

        const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
        glShaderSource(shader, 1, &source_ptr, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[4096];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            LOG_ERROR("Compute shader compile error (" + path + "): " + log);
            glDeleteShader(shader);
            return 0;
        }

        const GLuint program = glCreateProgram();
        glAttachShader(program, shader);
        glLinkProgram(program);
        glDeleteShader(shader);
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[4096];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            LOG_ERROR("Compute program link error (" + path + "): " + log);
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    // Новый SSBO заданного размера (байты); data может быть nullptr.
    unsigned int create_ssbo(size_t bytes, const void* data, GLenum usage) {
        unsigned int id = 0;
        glGenBuffers(1, &id);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, id);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(bytes), data, usage);
        return id;
    }
}

Renderer::Renderer()
    : m_shader_ptr(nullptr)
{}

Renderer::~Renderer() {
    for (auto& [key, res] : m_chunk_gl_cache) {
        delete_chunk_resources(res);
    }
    shutdown_gpu_mesher();

    delete m_shader_ptr;
    delete m_liquid_shader_ptr;
    delete m_flora_shader_ptr;
    delete m_sky_shader_ptr;
    delete m_occlusion_shader_ptr;
    delete m_mob_shader_ptr;
    delete m_item_shader_ptr;
    if (m_item_vao) glDeleteVertexArrays(1, &m_item_vao);
    if (m_item_vbo) glDeleteBuffers(1, &m_item_vbo);
    if (m_sky_quad_vao) glDeleteVertexArrays(1, &m_sky_quad_vao);
    if (m_sky_quad_vbo) glDeleteBuffers(1, &m_sky_quad_vbo);
    if (m_occlusion_cube_vao) glDeleteVertexArrays(1, &m_occlusion_cube_vao);
    if (m_occlusion_cube_vbo) glDeleteBuffers(1, &m_occlusion_cube_vbo);
}

bool Renderer::initialize(const std::string& vertex_shader_path,
                         const std::string& fragment_shader_path,
                         const std::string& blocks_atlas_name) {
    m_shader_ptr = new Shader(vertex_shader_path, fragment_shader_path);
    if (m_shader_ptr->get_id() == 0) {
        LOG_ERROR("Failed to create shader program");
        return false;
    }

    // Отдельный шейдер для жидкости (анимация волны/течения) — жёстко зашитый путь, как и
    // у HUD-шейдеров в Hud.cpp: это фиксированная пара файлов конкретно под liquid-проход,
    // а не параметр, который вызывающий код мог бы разумно захотеть подменить.
    m_liquid_shader_ptr = new Shader("assets/shaders/liquid_vertex.glsl", "assets/shaders/liquid_fragment.glsl");
    if (m_liquid_shader_ptr->get_id() == 0) {
        LOG_ERROR("Failed to create liquid shader program");
        return false;
    }

    // Отдельный шейдер для флоры (покачивание на ветру) — тот же принцип, что у liquid выше.
    m_flora_shader_ptr = new Shader("assets/shaders/flora_vertex.glsl", "assets/shaders/flora_fragment.glsl");
    if (m_flora_shader_ptr->get_id() == 0) {
        LOG_ERROR("Failed to create flora shader program");
        return false;
    }

    // Солнце/луна — процедурные billboard-диски, см. draw_sky().
    m_sky_shader_ptr = new Shader("assets/shaders/sky_object_vertex.glsl", "assets/shaders/sky_object_fragment.glsl");
    if (m_sky_shader_ptr->get_id() == 0) {
        LOG_ERROR("Failed to create sky shader program");
        return false;
    }

    // Отдельный минимальный шейдер для occlusion-запросов (см. issue_occlusion_query):
    // просто трансформирует вершины, ничего не сэмплит и не освещает — этот проход рисуется
    // с выключенными color/depth writes, единственная цель — занять GL_ANY_SAMPLES_PASSED
    // запрос геометрией bounding-бокса секции чанка.
    m_occlusion_shader_ptr = new Shader("assets/shaders/occlusion_vertex.glsl", "assets/shaders/occlusion_fragment.glsl");
    if (m_occlusion_shader_ptr->get_id() == 0) {
        LOG_ERROR("Failed to create occlusion shader program");
        return false;
    }

    // Скиннингованные мобы — см. src/mobs/Mob.cpp.
    m_mob_shader_ptr = new Shader("assets/shaders/mob_vertex.glsl", "assets/shaders/mob_fragment.glsl");
    if (m_mob_shader_ptr->get_id() == 0) {
        LOG_ERROR("Failed to create mob shader program");
        return false;
    }

    // Выпавшие предметы — основной вершинный шейдер + свой фрагментный с alpha-отсечением.
    m_item_shader_ptr = new Shader("assets/shaders/vertex.glsl", "assets/shaders/item_fragment.glsl");
    if (m_item_shader_ptr->get_id() == 0) {
        LOG_ERROR("Failed to create dropped item shader program");
        return false;
    }

    // Единичный куб (0..1 по каждой оси), который растягивается/двигается через uniform
    // "model" под AABB конкретной секции — так не нужно перезаливать VBO на каждый запрос.
    const float occlusion_cube[] = {
        // -X
        0,0,0,  0,1,0,  0,1,1,   0,0,0,  0,1,1,  0,0,1,
        // +X
        1,0,0,  1,1,1,  1,1,0,   1,0,0,  1,0,1,  1,1,1,
        // -Y
        0,0,0,  1,0,1,  1,0,0,   0,0,0,  0,0,1,  1,0,1,
        // +Y
        0,1,0,  1,1,0,  1,1,1,   0,1,0,  1,1,1,  0,1,1,
        // -Z
        0,0,0,  1,1,0,  1,0,0,   0,0,0,  0,1,0,  1,1,0,
        // +Z
        0,0,1,  1,0,1,  1,1,1,   0,0,1,  1,1,1,  0,1,1,
    };
    glGenVertexArrays(1, &m_occlusion_cube_vao);
    glGenBuffers(1, &m_occlusion_cube_vbo);
    glBindVertexArray(m_occlusion_cube_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_occlusion_cube_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(occlusion_cube), occlusion_cube, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    const float sky_quad[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f
    };
    glGenVertexArrays(1, &m_sky_quad_vao);
    glGenBuffers(1, &m_sky_quad_vbo);
    glBindVertexArray(m_sky_quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_sky_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(sky_quad), sky_quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Atlas_Registry::load_all() обязан был отработать до этого места (см. Game::initialize) —
    // сам Renderer атлас не грузит, только берёт готовый по имени и хранит указатель.
    m_blocks_atlas_ptr = Atlas_Registry::get_instance().get_atlas(blocks_atlas_name);
    if (!m_blocks_atlas_ptr) {
        LOG_ERROR("Failed to get blocks atlas \"" + blocks_atlas_name + "\" from Atlas_Registry");
        return false;
    }

    // GPU-мешер необязателен: без GL 4.3 или при ошибке шейдеров мешинг остаётся на CPU.
    initialize_gpu_mesher();

    LOG_INFO("Renderer initialized");
    return true;
}

void Renderer::clear_screen(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
}

void Renderer::upload_mesh(unsigned int& vao, unsigned int& vbo, size_t& count, const std::vector<Vertex>& vertices) {
    if (vertices.empty()) {
        if (vao != 0) glDeleteVertexArrays(1, &vao);
        if (vbo != 0) glDeleteBuffers(1, &vbo);
        vao = 0;
        vbo = 0;
        count = 0;
        return;
    }

    if (vbo == 0) {
        glGenBuffers(1, &vbo);
    }
    if (vao == 0) {
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(5 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(8 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glBindVertexArray(0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);
    count = vertices.size();
}

// Непрозрачная геометрия чанка (жадный меш): вершина Terrain_Vertex, см. terrain_vertex.glsl.
void Renderer::upload_terrain_mesh(unsigned int& vao, unsigned int& vbo, size_t& count,
                                   const std::vector<Terrain_Vertex>& vertices) {
    if (vertices.empty()) {
        if (vao != 0) glDeleteVertexArrays(1, &vao);
        if (vbo != 0) glDeleteBuffers(1, &vbo);
        vao = 0;
        vbo = 0;
        count = 0;
        return;
    }

    if (vbo == 0) {
        glGenBuffers(1, &vbo);
    }
    if (vao == 0) {
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        const GLsizei stride = sizeof(Terrain_Vertex);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Terrain_Vertex, x)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Terrain_Vertex, local_u)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Terrain_Vertex, ny)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Terrain_Vertex, light)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Terrain_Vertex, rect_u_min)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Terrain_Vertex, ao)));
        glEnableVertexAttribArray(5);
        glBindVertexArray(0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Terrain_Vertex), vertices.data(), GL_STATIC_DRAW);
    count = vertices.size();
}

void Renderer::render_dropped_items(const std::vector<Vertex>& vertices,
                                    const glm::mat4& view,
                                    const glm::mat4& projection,
                                    float ambient_intensity) {
    if (vertices.empty() || !m_item_shader_ptr) return;

    if (m_item_vao == 0) {
        glGenVertexArrays(1, &m_item_vao);
        glGenBuffers(1, &m_item_vbo);
        glBindVertexArray(m_item_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_item_vbo);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(5 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(8 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glBindVertexArray(0);
    }

    glBindVertexArray(m_item_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_item_vbo);
    if (vertices.size() > m_item_vbo_capacity) {
        // Растим с запасом, чтобы не переаллоцировать буфер каждый раз, когда падает ещё один предмет.
        m_item_vbo_capacity = vertices.size() * 2;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_item_vbo_capacity * sizeof(Vertex)),
                     nullptr, GL_DYNAMIC_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)), vertices.data());

    m_item_shader_ptr->use();
    m_item_shader_ptr->set_mat4("model", glm::mat4(1.0f));
    m_item_shader_ptr->set_mat4("view", view);
    m_item_shader_ptr->set_mat4("projection", projection);
    m_item_shader_ptr->set_int("textureAtlas", 0);
    m_item_shader_ptr->set_float("ambient", ambient_intensity);
    m_item_shader_ptr->set_float("maxLightLevel", static_cast<float>(Config::max_light_level));
    m_blocks_atlas_ptr->bind(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE); // спрайты двусторонние
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
}

void Renderer::draw_mesh(unsigned int vao, size_t count) {
    if (count == 0 || vao == 0) return;
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(count));
    m_last_rendered_triangle_count += count / 3;
}

void Renderer::draw_mesh_range(unsigned int vao, size_t start, size_t count) {
    if (count == 0 || vao == 0) return;
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, static_cast<GLint>(start), static_cast<GLsizei>(count));
    m_last_rendered_triangle_count += count / 3;
}

void Renderer::delete_chunk_resources(Chunk_GL_Resources& resources) {
    release_gpu_slot(resources);
    if (resources.solid_vao != 0) glDeleteVertexArrays(1, &resources.solid_vao);
    if (resources.flora_vao != 0) glDeleteVertexArrays(1, &resources.flora_vao);
    if (resources.liquid_vao != 0) glDeleteVertexArrays(1, &resources.liquid_vao);
    if (resources.solid_vbo != 0) glDeleteBuffers(1, &resources.solid_vbo);
    if (resources.flora_vbo != 0) glDeleteBuffers(1, &resources.flora_vbo);
    if (resources.liquid_vbo != 0) glDeleteBuffers(1, &resources.liquid_vbo);
    for (Section_Occlusion_State& state : resources.occlusion) {
        if (state.query != 0) glDeleteQueries(1, &state.query);
    }
}

void Renderer::poll_occlusion_result(Section_Occlusion_State& state) {
    if (!state.query_in_flight) return;

    GLuint available = 0;
    glGetQueryObjectuiv(state.query, GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available) {
        // Driver hasn't finished yet (rare with a 1-frame-old query) — keep last known
        // visibility and try again next frame rather than stalling the pipeline.
        return;
    }

    GLuint any_samples_passed = 0;
    glGetQueryObjectuiv(state.query, GL_QUERY_RESULT, &any_samples_passed);
    state.visible = (any_samples_passed != 0);
    state.query_in_flight = false;
}

void Renderer::issue_occlusion_query(Section_Occlusion_State& state, const glm::mat4& view,
                                     const glm::mat4& projection, const glm::vec3& min, const glm::vec3& max) {
    if (state.query == 0) {
        glGenQueries(1, &state.query);
    }
    // A query object can only have one query in flight at a time — if last frame's result
    // hasn't been read yet (poll_occlusion_result), starting a new one here would be
    // undefined. In practice poll runs first every frame so this just guards the edge case.
    if (state.query_in_flight) return;

    // This query runs AFTER the opaque pass has already written this section's own solid
    // geometry into the depth buffer (if it was visible last frame), so a naive test would
    // compare the box against itself. An earlier version of this function biased the box
    // *inward* (shrink + a depth-farther GL_POLYGON_OFFSET) so that self-comparison would
    // reliably FAIL. That turned out to be the wrong direction: for any section with no
    // *other* geometry sharing its screen footprint — most obviously a flat surface (ground,
    // a cave wall) filling the view with no sky or gap poking past its silhouette — the box
    // has nothing else to be tested against. Reliably failing its own test doesn't mean
    // "genuinely occluded", it just means "not drawn next frame" — which empties exactly the
    // depth region the query needs, so next frame's query reliably PASSES instead (nothing
    // left to fail against), so it draws again, fails again... a clean, deterministic
    // draw/hide/draw/hide loop every single frame, independent of camera movement (confirmed
    // by literally replaying the same frame N times with a static camera: fail-biased alternates
    // 0/1/0/1 forever; nothing about it depends on jitter). A real, independent occluder (a
    // hill genuinely in front of a cave section, a wall genuinely in front of another chunk)
    // never has this problem regardless of bias direction, because that depth comes from
    // *other* geometry that keeps getting redrawn every frame on its own merits — only the
    // self-comparison case is fragile.
    //
    // The fix is to bias the other way: grow the box slightly and push its tested depth
    // slightly CLOSER (negative GL_POLYGON_OFFSET), so a section's self-comparison reliably
    // PASSES (stays visible — correct, since by definition nothing else occupies that depth).
    // A genuine external occluder is never a hair's-breadth away — it's blocks closer — so a
    // margin of a few depth-buffer steps here never lets real occlusion slip through; verified
    // against a real closer box, which still hides the tested section every time. Polygon
    // offset (not just the geometric grow below) is what keeps this reliable at any distance
    // and viewing angle: it biases in the depth buffer's own quantization units rather than
    // world-space blocks, which would otherwise shrink to nothing at typical view distances
    // with this project's near/far (0.1 / 1000).
    constexpr float occlusion_box_bias = 0.05f; // negative = grow outward, biased toward "visible"
    const glm::vec3 inset_min = min - glm::vec3(occlusion_box_bias);
    const glm::vec3 inset_max = max + glm::vec3(occlusion_box_bias);

    const glm::vec3 size = glm::max(inset_max - inset_min, glm::vec3(0.01f));
    glm::mat4 model(1.0f);
    model = glm::translate(model, inset_min);
    model = glm::scale(model, size);

    m_occlusion_shader_ptr->set_mat4("model", model);
    m_occlusion_shader_ptr->set_mat4("view", view);
    m_occlusion_shader_ptr->set_mat4("projection", projection);

    glBeginQuery(GL_ANY_SAMPLES_PASSED, state.query);
    glBindVertexArray(m_occlusion_cube_vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glEndQuery(GL_ANY_SAMPLES_PASSED);
    state.query_in_flight = true;
}

bool Renderer::initialize_gpu_mesher() {
    // Диагностика: OPTICRAFT_CPU_MESH=1 выключает GPU-мешер, все чанки мешатся на CPU.
    // Сравнение картинки с этим флагом и без него показывает, в каком пути лежит ошибка.
    if (const char* forced = std::getenv("OPTICRAFT_CPU_MESH"); forced && forced[0] == '1') {
        LOG_INFO("GPU meshing disabled by OPTICRAFT_CPU_MESH=1");
        return false;
    }
    if (!GLAD_GL_VERSION_4_3) {
        LOG_INFO("GPU meshing disabled: OpenGL 4.3 is not available");
        return false;
    }

    m_gpu_greedy_program = compile_compute_program("assets/shaders/mesh_greedy.comp");
    m_gpu_finalize_program = compile_compute_program("assets/shaders/mesh_finalize.comp");
    m_gpu_terrain_shader_ptr = new Shader("assets/shaders/terrain_gpu_vertex.glsl",
                                          "assets/shaders/terrain_gpu_fragment.glsl");
    // Shader не бросает исключений при ошибке линковки, поэтому статус проверяем явно.
    GLint terrain_linked = GL_FALSE;
    if (m_gpu_terrain_shader_ptr->get_id() != 0) {
        glGetProgramiv(m_gpu_terrain_shader_ptr->get_id(), GL_LINK_STATUS, &terrain_linked);
    }
    if (m_gpu_greedy_program == 0 || m_gpu_finalize_program == 0 || terrain_linked != GL_TRUE) {
        LOG_ERROR("GPU meshing disabled: shader setup failed, meshing stays on CPU");
        shutdown_gpu_mesher();
        return false;
    }
    m_gpu_loc_chunk_count = glGetUniformLocation(m_gpu_greedy_program, "u_chunk_count");
    m_gpu_loc_greedy_quad_cap = glGetUniformLocation(m_gpu_greedy_program, "u_quad_cap");
    m_gpu_loc_section_total = glGetUniformLocation(m_gpu_finalize_program, "u_section_total");
    m_gpu_loc_finalize_quad_cap = glGetUniformLocation(m_gpu_finalize_program, "u_quad_cap");

    // Таблица правил и UV строится теми же resolve_face_uv/get_uv, что и CPU-мешер, поэтому
    // совпадение с CPU не зависит от того, что видеокарта "знает" о блоках сама.
    // LUT растёт с реально зарегистрированными id (а не с потолком 65536): 6 граней x 256 meta на id.
    const size_t id_count = Block_Registry::get_instance().id_capacity();
    std::vector<uint32_t> lut(id_count * 6 * 256, 0u);
    std::vector<glm::vec4> rects;
    std::map<std::array<float, 4>, uint32_t> rect_index;
    const auto intern_rect = [&rects, &rect_index](const glm::vec4& uv) -> uint32_t {
        const std::array<float, 4> key{uv.x, uv.y, uv.z, uv.w};
        const auto it = rect_index.find(key);
        if (it != rect_index.end()) return it->second;
        const uint32_t index = static_cast<uint32_t>(rects.size());
        rects.push_back(uv);
        rect_index.emplace(key, index);
        return index;
    };
    for (size_t id = 0; id < id_count; ++id) {
        const Block_Properties& props = get_block_props(static_cast<Block_ID>(id));
        for (int face = 0; face < 6; ++face) {
            for (int meta = 0; meta < 256; ++meta) {
                uint32_t entry = 0;
                if (props.uses_meta) {
                    bool rotate = false;
                    const glm::vec4& uv = props.resolve_face_uv(face, static_cast<uint8_t>(meta), rotate);
                    entry = intern_rect(uv) | (rotate ? 0x8000u : 0u);
                } else {
                    const int direction = face == 2 ? 1 : (face == 3 ? -1 : 0);
                    entry = intern_rect(props.get_uv(direction));
                }
                lut[(id * 6 + static_cast<size_t>(face)) * 256 + meta] = entry;
            }
        }
    }
    if (rects.size() > 0x7FFFu) {
        LOG_ERROR("GPU meshing disabled: too many UV rectangles for the 15-bit index");
        shutdown_gpu_mesher();
        return false;
    }

    const Gpu_Mesh_Tables tables = gpu_mesh_tables();
    std::vector<uint32_t> table_words;
    table_words.reserve(tables.flags.size() + tables.light_coarse.size());
    // Порядок как в шейдере: light_coarse (фиксированные 256 слов), затем флаги по id.
    table_words.insert(table_words.end(), tables.light_coarse.begin(), tables.light_coarse.end());
    table_words.insert(table_words.end(), tables.flags.begin(), tables.flags.end());

    m_gpu_lut_ssbo = create_ssbo(lut.size() * sizeof(uint32_t), lut.data(), GL_STATIC_DRAW);
    m_gpu_tables_ssbo = create_ssbo(table_words.size() * sizeof(uint32_t), table_words.data(), GL_STATIC_DRAW);
    m_gpu_rects_ssbo = create_ssbo(rects.size() * sizeof(glm::vec4), rects.data(), GL_STATIC_DRAW);
    m_gpu_headers_ssbo = create_ssbo(static_cast<size_t>(kGpuBatchChunks) * kGpuHeaderStride * sizeof(glm::uvec4), nullptr, GL_STREAM_DRAW);
    m_gpu_payload_capacity = size_t{1} << 20;
    m_gpu_payload_ssbo = create_ssbo(m_gpu_payload_capacity, nullptr, GL_STREAM_DRAW);
    for (Gpu_Readback& rb : m_gpu_readbacks) {
        rb.buffer = create_ssbo(static_cast<size_t>(kGpuBatchChunks) * 4 * sizeof(uint32_t), nullptr, GL_STREAM_READ);
    }
    glGenVertexArrays(1, &m_gpu_section_vao);
    grow_gpu_slots(256);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    m_gpu_enabled = true;
    LOG_INFO("GPU meshing enabled (OpenGL 4.3 compute, quad cap per section: " + std::to_string(kGpuQuadCap) + ")");
    return true;
}

void Renderer::shutdown_gpu_mesher() {
    for (Gpu_Readback& rb : m_gpu_readbacks) {
        if (rb.fence) glDeleteSync(rb.fence);
        if (rb.buffer) glDeleteBuffers(1, &rb.buffer);
        rb = Gpu_Readback{};
    }
    unsigned int buffers[] = {
        m_gpu_lut_ssbo, m_gpu_tables_ssbo, m_gpu_rects_ssbo, m_gpu_headers_ssbo, m_gpu_payload_ssbo,
        m_gpu_quads_ssbo, m_gpu_counts_ssbo, m_gpu_origins_ssbo, m_gpu_vis_ssbo, m_gpu_cmd_ssbo,
        m_gpu_section_ids_vbo
    };
    for (unsigned int id : buffers) {
        if (id != 0) glDeleteBuffers(1, &id);
    }
    if (m_gpu_section_vao != 0) glDeleteVertexArrays(1, &m_gpu_section_vao);
    if (m_gpu_greedy_program != 0) glDeleteProgram(m_gpu_greedy_program);
    if (m_gpu_finalize_program != 0) glDeleteProgram(m_gpu_finalize_program);
    delete m_gpu_terrain_shader_ptr;

    m_gpu_terrain_shader_ptr = nullptr;
    m_gpu_greedy_program = 0;
    m_gpu_finalize_program = 0;
    m_gpu_section_vao = 0;
    m_gpu_section_ids_vbo = 0;
    m_gpu_lut_ssbo = m_gpu_tables_ssbo = m_gpu_rects_ssbo = 0;
    m_gpu_headers_ssbo = m_gpu_payload_ssbo = 0;
    m_gpu_quads_ssbo = m_gpu_counts_ssbo = m_gpu_origins_ssbo = m_gpu_vis_ssbo = m_gpu_cmd_ssbo = 0;
    m_gpu_payload_capacity = 0;
    m_gpu_slot_capacity = 0;
    m_gpu_slot_high_water = 0;
    m_gpu_free_slots.clear();
    m_gpu_vis_cpu.clear();
    m_gpu_quad_counts.clear();
    m_gpu_headers_cpu.clear();
    m_gpu_payload_cpu.clear();
    m_gpu_readback_head = 0;
    m_gpu_readback_count = 0;
    m_gpu_fallback_requests.clear();
    m_gpu_enabled = false;
}

void Renderer::grow_gpu_slots(int needed) {
    if (needed <= m_gpu_slot_capacity) return;
    const int new_capacity = std::max(needed, std::max(256, m_gpu_slot_capacity * 2));
    const size_t capacity = static_cast<size_t>(new_capacity);
    const size_t old_capacity = static_cast<size_t>(m_gpu_slot_capacity);
    const size_t quad_bytes_per_slot = static_cast<size_t>(4) * kGpuQuadCap * sizeof(glm::uvec2);

    const std::vector<uint32_t> zero_counts(4 * capacity, 0u);
    const std::vector<glm::vec4> zero_origins(capacity, glm::vec4(0.0f));
    const unsigned int quads = create_ssbo(quad_bytes_per_slot * capacity, nullptr, GL_DYNAMIC_DRAW);
    const unsigned int counts = create_ssbo(zero_counts.size() * sizeof(uint32_t), zero_counts.data(), GL_DYNAMIC_DRAW);
    const unsigned int origins = create_ssbo(zero_origins.size() * sizeof(glm::vec4), zero_origins.data(), GL_DYNAMIC_DRAW);
    const unsigned int vis = create_ssbo(capacity * sizeof(uint32_t), nullptr, GL_DYNAMIC_DRAW);
    const unsigned int cmd = create_ssbo(4 * capacity * sizeof(glm::uvec4), nullptr, GL_DYNAMIC_DRAW);
    // Тождественные номера секций для атрибута aSection (см. terrain_gpu_vertex.glsl).
    std::vector<uint32_t> section_ids(4 * capacity);
    std::iota(section_ids.begin(), section_ids.end(), 0u);
    unsigned int ids = 0;
    glGenBuffers(1, &ids);
    glBindBuffer(GL_ARRAY_BUFFER, ids);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(section_ids.size() * sizeof(uint32_t)),
                 section_ids.data(), GL_STATIC_DRAW);

    if (old_capacity > 0) {
        // Между кадрами живут квады, счётчики и origins: переносим их в новые буферы.
        const auto copy_bytes = [](unsigned int src, unsigned int dst, size_t bytes) {
            glBindBuffer(GL_COPY_READ_BUFFER, src);
            glBindBuffer(GL_COPY_WRITE_BUFFER, dst);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, static_cast<GLsizeiptr>(bytes));
        };
        copy_bytes(m_gpu_quads_ssbo, quads, quad_bytes_per_slot * old_capacity);
        copy_bytes(m_gpu_counts_ssbo, counts, static_cast<size_t>(4) * old_capacity * sizeof(uint32_t));
        copy_bytes(m_gpu_origins_ssbo, origins, old_capacity * sizeof(glm::vec4));
    }

    unsigned int old_buffers[] = {m_gpu_quads_ssbo, m_gpu_counts_ssbo, m_gpu_origins_ssbo, m_gpu_vis_ssbo, m_gpu_cmd_ssbo};
    for (unsigned int old_id : old_buffers) {
        if (old_id != 0) glDeleteBuffers(1, &old_id);
    }
    m_gpu_quads_ssbo = quads;
    m_gpu_counts_ssbo = counts;
    m_gpu_origins_ssbo = origins;
    m_gpu_vis_ssbo = vis;
    m_gpu_cmd_ssbo = cmd;
    if (m_gpu_section_ids_vbo != 0) glDeleteBuffers(1, &m_gpu_section_ids_vbo);
    m_gpu_section_ids_vbo = ids;
    glBindVertexArray(m_gpu_section_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_gpu_section_ids_vbo);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(uint32_t), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribDivisor(0, 1);
    glBindVertexArray(0);
    m_gpu_slot_capacity = new_capacity;
    m_gpu_vis_cpu.resize(capacity, 0u);
    m_gpu_quad_counts.resize(4 * capacity, 0u);
}

int Renderer::acquire_gpu_slot() {
    if (!m_gpu_free_slots.empty()) {
        const int slot = m_gpu_free_slots.back();
        m_gpu_free_slots.pop_back();
        return slot;
    }
    const int slot = m_gpu_slot_high_water;
    grow_gpu_slots(slot + 1);
    ++m_gpu_slot_high_water;
    return slot;
}

void Renderer::release_gpu_slot(Chunk_GL_Resources& resources) {
    if (resources.gpu_slot < 0) return;
    m_gpu_free_slots.push_back(resources.gpu_slot);
    resources.gpu_slot = -1;
    resources.gpu_version = 0;
}

void Renderer::poll_gpu_readbacks(bool wait_for_oldest) {
    while (m_gpu_readback_count > 0) {
        Gpu_Readback& rb = m_gpu_readbacks[m_gpu_readback_head];
        // Ждём только самый старый и только по запросу; остальные проверяем без блокировки.
        const GLenum status = glClientWaitSync(rb.fence, GL_SYNC_FLUSH_COMMANDS_BIT,
                                               wait_for_oldest ? 1000000000ull : 0ull);
        wait_for_oldest = false;
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) break;

        const size_t count = rb.positions.size();
        glBindBuffer(GL_COPY_READ_BUFFER, rb.buffer);
        const auto* words = static_cast<const uint32_t*>(glMapBufferRange(
            GL_COPY_READ_BUFFER, 0, static_cast<GLsizeiptr>(count * 4 * sizeof(uint32_t)), GL_MAP_READ_BIT));
        if (words) {
            for (size_t i = 0; i < count; ++i) {
                const int slot = rb.slots[i];
                bool overflow = false;
                for (size_t s = 0; s < 4; ++s) {
                    const uint32_t quads = words[i * 4 + s];
                    m_gpu_quad_counts[static_cast<size_t>(slot) * 4 + s] = quads;
                    overflow = overflow || quads > static_cast<uint32_t>(kGpuQuadCap);
                }
                if (!overflow) continue;
                const auto it = m_chunk_gl_cache.find(rb.positions[i]);
                if (it == m_chunk_gl_cache.end()) continue;
                // Отчёт относится к квадам именно этой версии в этом слоте; иначе он устарел.
                if (it->second.gpu_slot != slot || it->second.gpu_version != rb.versions[i]) continue;
                if (std::find(m_gpu_fallback_requests.begin(), m_gpu_fallback_requests.end(),
                              rb.positions[i]) == m_gpu_fallback_requests.end()) {
                    m_gpu_fallback_requests.push_back(rb.positions[i]);
                }
            }
            glUnmapBuffer(GL_COPY_READ_BUFFER);
        }

        glDeleteSync(rb.fence);
        rb.fence = nullptr;
        rb.positions.clear();
        rb.versions.clear();
        rb.slots.clear();
        m_gpu_readback_head = (m_gpu_readback_head + 1) % kGpuReadbackRing;
        --m_gpu_readback_count;
    }
}

void Renderer::submit_gpu_batch(const std::vector<const Renderable_Chunk*>& batch) {
    const size_t n = batch.size();
    m_gpu_headers_cpu.assign(n * kGpuHeaderStride, glm::uvec4(0u));
    m_gpu_payload_cpu.clear();
    Gpu_Readback& rb = m_gpu_readbacks[(m_gpu_readback_head + m_gpu_readback_count) % kGpuReadbackRing];

    for (size_t i = 0; i < n; ++i) {
        const Renderable_Chunk& chunk = *batch[i];
        const Chunk_Meshes& meshes = *chunk.meshes;
        Chunk_GL_Resources& res = m_chunk_gl_cache[chunk.position];
        if (res.gpu_slot < 0) res.gpu_slot = acquire_gpu_slot();
        res.gpu_version = chunk.mesh_version;

        // Палитры и слова чанка кладём в общий батч; смещения переносим на его начало.
        const uint32_t base = static_cast<uint32_t>(m_gpu_payload_cpu.size());
        m_gpu_payload_cpu.insert(m_gpu_payload_cpu.end(), meshes.gpu_words.begin(), meshes.gpu_words.end());

        // Заголовок чанка: [0].x — слот, [1 + канал] — палитра, слова, битность.
        glm::uvec4* header = &m_gpu_headers_cpu[i * kGpuHeaderStride];
        header[0] = glm::uvec4(static_cast<uint32_t>(res.gpu_slot), 0u, 0u, 0u);
        for (int ch = 0; ch < kGpuMeshChannelCount; ++ch) {
            const std::array<uint32_t, 3>& c = meshes.gpu_channels[ch];
            header[1 + ch] = glm::uvec4(base + c[0], base + c[1], c[2], 0u);
        }

        // Мировой угол чанка для вершинного шейдера и обнуление счётчиков секций слота.
        const glm::vec4 origin(static_cast<float>(chunk.position.x * Config::chunk_size),
                               static_cast<float>(chunk.position.y * Config::chunk_height),
                               static_cast<float>(chunk.position.z * Config::chunk_size), 0.0f);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_origins_ssbo);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(res.gpu_slot) * sizeof(glm::vec4),
                        sizeof(glm::vec4), &origin);
        const uint32_t zero_counts[4] = {0u, 0u, 0u, 0u};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_counts_ssbo);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(res.gpu_slot) * 4 * sizeof(uint32_t),
                        sizeof(zero_counts), zero_counts);

        rb.positions.push_back(chunk.position);
        rb.versions.push_back(chunk.mesh_version);
        rb.slots.push_back(res.gpu_slot);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_headers_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(n * kGpuHeaderStride * sizeof(glm::uvec4)),
                    m_gpu_headers_cpu.data());
    const size_t payload_bytes = m_gpu_payload_cpu.size() * sizeof(uint32_t);
    if (payload_bytes > m_gpu_payload_capacity) {
        m_gpu_payload_capacity = payload_bytes * 2;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_payload_ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(m_gpu_payload_capacity), nullptr, GL_STREAM_DRAW);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_payload_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(payload_bytes), m_gpu_payload_cpu.data());

    // Greedy: 576 работ на чанк (72 среза x 2 знака x 4 секции), по 64 в группе = 9 групп на чанк.
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    glUseProgram(m_gpu_greedy_program);
    glUniform1i(m_gpu_loc_chunk_count, static_cast<GLint>(n));
    glUniform1i(m_gpu_loc_greedy_quad_cap, kGpuQuadCap);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_gpu_headers_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_gpu_payload_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_gpu_quads_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_gpu_counts_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_gpu_lut_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_gpu_tables_ssbo);
    glDispatchCompute(static_cast<GLuint>(n) * 9u, 1, 1);

    // Счётчики квадов уходят в readback: по ним узнаём переполнение, не останавливая конвейер.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_COPY_READ_BUFFER, m_gpu_counts_ssbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, rb.buffer);
    for (size_t i = 0; i < n; ++i) {
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                            static_cast<GLintptr>(rb.slots[i]) * 4 * sizeof(uint32_t),
                            static_cast<GLintptr>(i) * 4 * sizeof(uint32_t),
                            4 * sizeof(uint32_t));
    }
    rb.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    ++m_gpu_readback_count;
}

std::vector<glm::ivec3> Renderer::take_cpu_fallback_requests() {
    std::vector<glm::ivec3> requests;
    requests.swap(m_gpu_fallback_requests);
    return requests;
}

bool Renderer::debug_gpu_section_quads(const glm::ivec3& chunk_position, int section,
                                       std::vector<glm::uvec2>& out) {
    out.clear();
    const auto it = m_chunk_gl_cache.find(chunk_position);
    if (!m_gpu_enabled || it == m_chunk_gl_cache.end() || it->second.gpu_slot < 0) return false;
    const size_t index = static_cast<size_t>(it->second.gpu_slot) * 4 + static_cast<size_t>(section);
    const size_t count = std::min(static_cast<size_t>(m_gpu_quad_counts[index]),
                                  static_cast<size_t>(kGpuQuadCap));
    out.resize(count);
    if (count > 0) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_quads_ssbo);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                           static_cast<GLintptr>(index * kGpuQuadCap * sizeof(glm::uvec2)),
                           static_cast<GLsizeiptr>(count * sizeof(glm::uvec2)), out.data());
    }
    return true;
}

void Renderer::draw_solid_gpu(const std::vector<const Renderable_Chunk*>& chunks,
                              const glm::mat4& view,
                              const glm::mat4& projection,
                              float ambient_intensity) {
    if (!m_gpu_enabled || chunks.empty()) return;
    poll_gpu_readbacks(false);

    // Чанки без GPU-нагрузки или уже ушедшие на CPU освобождают слот; остальным, у кого нет
    // актуальных квадов, нужна загрузка. chunks уже отсортированы от ближних к дальним.
    std::vector<const Renderable_Chunk*> to_submit;
    for (const Renderable_Chunk* chunk_ptr : chunks) {
        const Renderable_Chunk& chunk = *chunk_ptr;
        Chunk_GL_Resources& res = m_chunk_gl_cache[chunk.position];
        const Chunk_Meshes& meshes = *chunk.meshes;
        if (!meshes.solid_on_gpu || !meshes.gpu_upload_pending) {
            release_gpu_slot(res);
            continue;
        }
        if (res.gpu_slot < 0 || res.gpu_version != chunk.mesh_version) {
            to_submit.push_back(chunk_ptr);
        }
    }

    if (!to_submit.empty()) {
        if (m_gpu_readback_count == kGpuReadbackRing) poll_gpu_readbacks(true);
        if (m_gpu_readback_count < kGpuReadbackRing) {
            const size_t batch_size = std::min(to_submit.size(), static_cast<size_t>(kGpuBatchChunks));
            submit_gpu_batch(std::vector<const Renderable_Chunk*>(to_submit.begin(), to_submit.begin() + batch_size));
        }
    }

    // Видимость: бит s слота — секция s попала во фрустум и в ней есть геометрия. Пустые по квадам
    // секции финализатор отбрасывает сам (count = 0), поэтому здесь счётчики не нужны.
    std::fill(m_gpu_vis_cpu.begin(), m_gpu_vis_cpu.begin() + m_gpu_slot_high_water, 0u);
    bool any_visible = false;
    for (const Renderable_Chunk* chunk_ptr : chunks) {
        const Renderable_Chunk& chunk = *chunk_ptr;
        const Chunk_GL_Resources& res = m_chunk_gl_cache.at(chunk.position);
        const Chunk_Meshes& meshes = *chunk.meshes;
        if (res.gpu_slot < 0 || !meshes.solid_on_gpu || !meshes.gpu_upload_pending) continue;

        uint32_t bits = 0;
        for (size_t s = 0; s < meshes.sections.size(); ++s) {
            if (!meshes.sections[s].has_geometry || !chunk.section_visible[s]) continue;
            bits |= 1u << s;
            // Статистика: число квадов берём из последнего полученного readback'а (с задержкой).
            const uint32_t quads = std::min(m_gpu_quad_counts[static_cast<size_t>(res.gpu_slot) * 4 + s],
                                            static_cast<uint32_t>(kGpuQuadCap));
            ++m_last_rendered_section_count;
            m_last_rendered_triangle_count += static_cast<size_t>(quads) * 2;
        }
        m_gpu_vis_cpu[res.gpu_slot] = bits;
        if (bits != 0) {
            any_visible = true;
            ++m_last_rendered_chunk_count;
        }
    }
    if (!any_visible) return;

    // Финализация: команда DrawArraysIndirect на каждую секцию пула (count = квады * 6, если видна).
    const int total_sections = m_gpu_slot_high_water * 4;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_vis_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(m_gpu_slot_high_water * sizeof(uint32_t)),
                    m_gpu_vis_cpu.data());
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glUseProgram(m_gpu_finalize_program);
    glUniform1i(m_gpu_loc_section_total, total_sections);
    glUniform1i(m_gpu_loc_finalize_quad_cap, kGpuQuadCap);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_gpu_counts_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_gpu_vis_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_gpu_cmd_ssbo);
    glDispatchCompute(static_cast<GLuint>((total_sections + 63) / 64), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

    // Отрисовка: вершины разворачиваются из квадов в вершинном шейдере, атрибутов нет.
    m_gpu_terrain_shader_ptr->use();
    m_gpu_terrain_shader_ptr->set_mat4("model", glm::mat4(1.0f));
    m_gpu_terrain_shader_ptr->set_mat4("view", view);
    m_gpu_terrain_shader_ptr->set_mat4("projection", projection);
    m_gpu_terrain_shader_ptr->set_int("u_quad_cap", kGpuQuadCap);
    m_gpu_terrain_shader_ptr->set_int("textureAtlas", 0);
    m_gpu_terrain_shader_ptr->set_float("ambient", ambient_intensity);
    m_gpu_terrain_shader_ptr->set_float("maxLightLevel", static_cast<float>(Config::max_light_level));
    m_blocks_atlas_ptr->bind(0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_gpu_quads_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, m_gpu_rects_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, m_gpu_origins_ssbo);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_gpu_cmd_ssbo);
    glBindVertexArray(m_gpu_section_vao);
    glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, total_sections, 0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindVertexArray(0);
}

void Renderer::render_chunks(const std::vector<Renderable_Chunk>& chunks,
                            const glm::mat4& view,
                            const glm::mat4& projection,
                            float ambient_intensity,
                            bool spectator) {
    m_last_rendered_chunk_count = 0;
    m_last_rendered_triangle_count = 0;
    m_last_rendered_section_count = 0;
    m_last_culled_section_count = 0;
    m_shader_ptr->use();
    m_shader_ptr->set_mat4("model", glm::mat4(1.0f));
    m_shader_ptr->set_mat4("view", view);
    m_shader_ptr->set_mat4("projection", projection);
    m_shader_ptr->set_int("textureAtlas", 0);
    m_shader_ptr->set_float("ambient", ambient_intensity);
    m_shader_ptr->set_float("maxLightLevel", static_cast<float>(Config::max_light_level));
    m_blocks_atlas_ptr->bind(0);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    m_last_occluded_section_count = 0;

    std::vector<const Renderable_Chunk*> transparent_chunks;
    transparent_chunks.reserve(chunks.size());

    const glm::vec3 camera_position = glm::vec3(glm::inverse(view)[3]);

    // Occlusion queries are order-dependent within a single frame: a section can only be
    // detected as hidden by geometry that has already written to the depth buffer. Sorting
    // opaque chunks front-to-back (nearest first) means real occluders — the terrain/cave
    // walls actually blocking the view — get drawn before we test what's behind them, so
    // the query for a deep, sealed-off cave section correctly fails once the mountain in
    // front of it has already been rasterized this same frame.
    std::vector<const Renderable_Chunk*> opaque_order;
    opaque_order.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        if (chunk.meshes) opaque_order.push_back(&chunk);
    }
    std::stable_sort(opaque_order.begin(), opaque_order.end(),
        [&camera_position](const Renderable_Chunk* lhs, const Renderable_Chunk* rhs) {
            const auto chunk_center = [](const Renderable_Chunk& c) {
                return glm::vec3(
                    (static_cast<float>(c.position.x) + 0.5f) * Config::chunk_size,
                    (static_cast<float>(c.position.y) + 0.5f) * Config::chunk_height,
                    (static_cast<float>(c.position.z) + 0.5f) * Config::chunk_size
                );
            };
            const glm::vec3 lhs_offset = chunk_center(*lhs) - camera_position;
            const glm::vec3 rhs_offset = chunk_center(*rhs) - camera_position;
            return glm::dot(lhs_offset, lhs_offset) < glm::dot(rhs_offset, rhs_offset);
        });

    for (const Renderable_Chunk* chunk_ptr : opaque_order) {
        const Renderable_Chunk& chunk = *chunk_ptr;
        auto& gl_res = m_chunk_gl_cache[chunk.position];

        if (!gl_res.is_valid || gl_res.mesh_version != chunk.mesh_version) {
            upload_terrain_mesh(gl_res.solid_vao, gl_res.solid_vbo, gl_res.solid_count, chunk.meshes->solid);
            upload_mesh(gl_res.flora_vao, gl_res.flora_vbo, gl_res.flora_count, chunk.meshes->flora);
            upload_mesh(gl_res.liquid_vao, gl_res.liquid_vbo, gl_res.liquid_count, chunk.meshes->liquid);
            gl_res.mesh_version = chunk.mesh_version;
            gl_res.is_valid = true;
        }

        // Секции по Y (см. Chunk_Section_Range): X/Z-границы у всех секций чанка одни и те
        // же, отличается только Y-диапазон — так что можно отсечь верхние/нижние секции,
        // не попадающие во фрустум (например, при взгляде почти горизонтально "небо" чанка
        // выше игрока часто не нужно рисовать), без отдельных VAO/VBO на секцию.
        const glm::vec3 footprint_min(
            static_cast<float>(chunk.position.x * Config::chunk_size),
            static_cast<float>(chunk.position.y * Config::chunk_height),
            static_cast<float>(chunk.position.z * Config::chunk_size)
        );
        const glm::vec3 footprint_max(
            footprint_min.x + static_cast<float>(Config::chunk_size), 0.0f,
            footprint_min.z + static_cast<float>(Config::chunk_size)
        );

        bool any_section_drawn = false;
        for (size_t section_index = 0; section_index < chunk.meshes->sections.size(); ++section_index) {
            const Chunk_Section_Range& section = chunk.meshes->sections[section_index];
            if (!section.has_geometry || section.solid_count == 0 ||
                !chunk.section_visible[section_index]) {
                if (section.has_geometry && section.solid_count > 0 &&
                    !chunk.section_visible[section_index]) {
                    ++m_last_culled_section_count;
                }
                continue;
            }

            const glm::vec3 section_min(footprint_min.x, section.min_y, footprint_min.z);
            const glm::vec3 section_max(footprint_max.x, section.max_y, footprint_max.z);

            // section.min_y/max_y is a tight bound over the WHOLE 32-wide section (every
            // vertex in it), not over whatever's directly underfoot — a hill on one edge of
            // the section pulls max_y up even for a player standing on lower ground elsewhere
            // in the same section. That can put the camera literally inside this box's Y range
            // while standing on real, solid, unrelated ground. An occlusion query has no good
            // answer there: looking straight down from inside the box reaches neither its side
            // walls (16 blocks off-center) nor its top (behind/above) — only its own distant
            // underside, far below where the real ground actually is, which is exactly the
            // self-referential setup that flickers (see issue_occlusion_query). So: skip the
            // query question entirely here and just draw it — a section containing the camera
            // is definitionally visible, no test needed.
            const bool camera_inside_section =
                camera_position.x >= section_min.x && camera_position.x <= section_max.x &&
                camera_position.y >= section_min.y && camera_position.y <= section_max.y &&
                camera_position.z >= section_min.z && camera_position.z <= section_max.z;

            Section_Occlusion_State& occ = gl_res.occlusion[section_index];
            if (m_occlusion_enabled && !spectator) {
                poll_occlusion_result(occ);
                if (camera_inside_section) occ.visible = true;
            }

            if (spectator || !m_occlusion_enabled || occ.visible) {
                draw_mesh_range(gl_res.solid_vao, section.solid_start, section.solid_count);
                ++m_last_rendered_section_count;
                any_section_drawn = true;
            } else {
                ++m_last_occluded_section_count;
            }
        }

        if (any_section_drawn) {
            ++m_last_rendered_chunk_count;
        }
        transparent_chunks.push_back(chunk_ptr);
    }

    // GPU-путь непрозрачной геометрии: тот же проход, что и CPU-солид, но квады строит видеокарта.
    draw_solid_gpu(opaque_order, view, projection, ambient_intensity);

    // Second pass: now that this frame's opaque draw has written real occluder depth,
    // test every section selected by Chunk_Manager against it. This is what
    // decides whether each section gets drawn NEXT frame — sections whose box is fully
    // behind other terrain (a sealed cave under a hill, the far side of a mountain, etc.)
    // come back "not visible" and are skipped outright, instead of being meshed and drawn
    // for geometry the camera can never actually see. Kept as its own pass (rather than
    // inline in the loop above) so occlusion is judged against the whole frame's opaque
    // geometry, not just whatever had been drawn so far.
    //
    // A section's own just-drawn geometry ends up in that same depth buffer, so its own box
    // inevitably gets tested against itself wherever nothing else shares its screen footprint
    // (see issue_occlusion_query for why that comparison is biased to reliably resolve as
    // "visible", not "occluded"). That bias alone isn't enough when the camera is actually
    // *inside* a section's box, though — section.min_y/max_y bounds the whole 32-wide section,
    // so a hill on one side of it can put the box's top well above a player standing on lower
    // ground elsewhere in the same section, and looking straight down from there reaches
    // neither the box's side walls (a dozen-plus blocks off-center) nor its top (behind/above)
    // — only its own far underside, well below the real ground. Both passes below special-case
    // this: a section containing the camera is drawn and marked visible unconditionally, no
    // query asked.
    //
    // Two extra bits of state matter here and nowhere else in the frame:
    //  - GL_CULL_FACE must be off. The section the camera is currently standing in/near
    //    (or inside a cave room) almost always contains the camera within its box. Every
    //    cube face is then back-facing from the inside and would be culled entirely, so
    //    the query would rasterize zero fragments and report "occluded" regardless of
    //    actual visibility — exactly the section under/around the player popping in and
    //    out as they move across a section boundary. An occlusion proxy has no "correct"
    //    winding to cull; both sides must render.
    //  - GL_POLYGON_OFFSET_FILL biases depth in the depth buffer's own units (see
    //    issue_occlusion_query for why the bias is negative — push toward the camera, not
    //    away), not world-space units, so unlike a geometric grow alone it stays effective
    //    regardless of distance from the camera.
    if (m_occlusion_enabled && !spectator) {
    m_occlusion_shader_ptr->use();
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-4.0f, -8.0f);
    for (const Renderable_Chunk* chunk_ptr : opaque_order) {
        const Renderable_Chunk& chunk = *chunk_ptr;
        auto& gl_res = m_chunk_gl_cache[chunk.position];
        const glm::vec3 footprint_min(
            static_cast<float>(chunk.position.x * Config::chunk_size),
            static_cast<float>(chunk.position.y * Config::chunk_height),
            static_cast<float>(chunk.position.z * Config::chunk_size)
        );
        const glm::vec3 footprint_max(
            footprint_min.x + static_cast<float>(Config::chunk_size), 0.0f,
            footprint_min.z + static_cast<float>(Config::chunk_size)
        );

        for (size_t section_index = 0; section_index < chunk.meshes->sections.size(); ++section_index) {
            const Chunk_Section_Range& section = chunk.meshes->sections[section_index];
            if (!section.has_geometry || section.solid_count == 0 ||
                !chunk.section_visible[section_index]) continue;

            const glm::vec3 section_min(footprint_min.x, section.min_y, footprint_min.z);
            const glm::vec3 section_max(footprint_max.x, section.max_y, footprint_max.z);

            // Same "camera embedded in this section's box" case as the first pass — skip
            // issuing a query for it (an already-visible query the poll would just read as
            // true anyway; a stray one left in flight from before the camera entered is
            // harmless, it'll be polled once it lands and ignored while this holds) and force
            // the state a query would need many frames to converge back to.
            const bool camera_inside_section =
                camera_position.x >= section_min.x && camera_position.x <= section_max.x &&
                camera_position.y >= section_min.y && camera_position.y <= section_max.y &&
                camera_position.z >= section_min.z && camera_position.z <= section_max.z;
            if (camera_inside_section) {
                gl_res.occlusion[section_index].visible = true;
                continue;
            }

            issue_occlusion_query(gl_res.occlusion[section_index], view, projection, section_min, section_max);
        }
    }
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    } // m_occlusion_enabled
    m_shader_ptr->use();
    std::stable_sort(transparent_chunks.begin(), transparent_chunks.end(),
        [&camera_position](const Renderable_Chunk* lhs, const Renderable_Chunk* rhs) {
            const auto chunk_center = [](const Renderable_Chunk& chunk) {
                return glm::vec3(
                    (static_cast<float>(chunk.position.x) + 0.5f) * Config::chunk_size,
                    (static_cast<float>(chunk.position.y) + 0.5f) * Config::chunk_height,
                    (static_cast<float>(chunk.position.z) + 0.5f) * Config::chunk_size
                );
            };
            const glm::vec3 lhs_offset = chunk_center(*lhs) - camera_position;
            const glm::vec3 rhs_offset = chunk_center(*rhs) - camera_position;
            return glm::dot(lhs_offset, lhs_offset) > glm::dot(rhs_offset, rhs_offset);
        });

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_TRUE);

    // Прозрачные проходы сортируются по чанку целиком (как и раньше), а не по секции —
    // сортировка секций внутри уже отсортированных по глубине чанков дала бы минимальный
    // выигрыш ценой куда более сложного кода, поэтому здесь секционный culling применяется
    // только для отсечения (пропуска), но не переупорядочивает вывод внутри чанка.
    // Флора — отдельный шейдер (покачивание верхних вершин на ветру, см.
    // assets/shaders/flora_*.glsl), по той же схеме, что и у liquid ниже.
    m_flora_shader_ptr->use();
    m_flora_shader_ptr->set_mat4("model", glm::mat4(1.0f));
    m_flora_shader_ptr->set_mat4("view", view);
    m_flora_shader_ptr->set_mat4("projection", projection);
    m_flora_shader_ptr->set_int("textureAtlas", 0);
    m_flora_shader_ptr->set_float("time", static_cast<float>(glfwGetTime()));
    m_flora_shader_ptr->set_float("ambient", ambient_intensity);
    m_flora_shader_ptr->set_float("maxLightLevel", static_cast<float>(Config::max_light_level));

    // Флора — крестообразные квады, у каждого ОДНА лицевая сторона (см.
    // add_flora_block_data в Chunk.cpp). При включённом GL_CULL_FACE каждый квад
    // отбрасывался с одной из двух сторон, и цветок было видно только с двух
    // направлений из четырёх. На время прохода флоры отсечение выключается,
    // и обе стороны квада попадают на экран. Освещение от этого не страдает:
    // шейдер флоры смотрит только на Normal.y, одинаковый для обеих сторон.
    glDisable(GL_CULL_FACE);

    // Дальность прорисовки флоры. Трава и цветы — самая мелкая и самая
    // многочисленная геометрия в мире, вдалеке она всё равно вырождается в шум
    // из отдельных пикселей, поэтому дальние чанки просто пропускаются.
    // Отрицательное значение = ограничения нет.
    for (const Renderable_Chunk* chunk : transparent_chunks) {
        if (chunk->flora_skip_render) continue;
        const auto& gl_res = m_chunk_gl_cache.at(chunk->position);
        for (size_t section_index = 0; section_index < chunk->meshes->sections.size(); ++section_index) {
            const Chunk_Section_Range& section = chunk->meshes->sections[section_index];
            if (!section.has_geometry || section.flora_count == 0 ||
                !chunk->section_visible[section_index]) continue;
            draw_mesh_range(gl_res.flora_vao, section.flora_start, section.flora_count);
        }
    }

    // Наблюдатель может оказаться внутри сплошного блока: у поверхности блока
    // обращённая внутрь сторона является обратной гранью. Оставляем её видимой,
    // иначе при полёте сквозь блок камера видит сквозь его стенки.
    glEnable(GL_CULL_FACE);

    // Жидкость — отдельный шейдер (анимация волны на вершинах + мерцание/скролл течения
    // на фрагментах, см. assets/shaders/liquid_*.glsl). glfwGetTime() — тот же самый общий
    // счётчик времени, что использует Game::run() для delta_time, поэтому не понадобилось
    // заводить отдельное поле "накопленного времени" только ради этого шейдера.
    m_liquid_shader_ptr->use();
    m_liquid_shader_ptr->set_mat4("model", glm::mat4(1.0f));
    m_liquid_shader_ptr->set_mat4("view", view);
    m_liquid_shader_ptr->set_mat4("projection", projection);
    m_liquid_shader_ptr->set_int("textureAtlas", 0);
    m_liquid_shader_ptr->set_float("time", static_cast<float>(glfwGetTime()));
    // tile_size больше не "1/сетка атласа целиком" (спрайты теперь разного размера) —
    // считаем размер именно того прямоугольника, который в UV занимает текстура воды,
    // чтобы скролл/мерцание в шейдере не выходили за её собственный кусок атласа.
    const glm::vec4& water_uv = get_block_props(Block_Types::Water).uv_side;
    m_liquid_shader_ptr->set_vec2("tile_size", glm::vec2(water_uv.z - water_uv.x, water_uv.w - water_uv.y));
    m_liquid_shader_ptr->set_float("ambient", ambient_intensity);
    m_liquid_shader_ptr->set_float("maxLightLevel", static_cast<float>(Config::max_light_level));

    // Liquid is alpha-blended and sorted back-to-front. It must NOT write depth: a far
    // water surface drawn first would otherwise stamp its depth into the buffer and make
    // the nearer surface fail GL_LESS, which is exactly the "far block covers near block"
    // artifact seen with overlapping semi-transparent blocks. Flora above remains a
    // depth-writing alpha-cutout pass, so solid-looking leaves/grass still occlude correctly.
    glDepthMask(GL_FALSE);

    for (const Renderable_Chunk* chunk : transparent_chunks) {
        const auto& gl_res = m_chunk_gl_cache.at(chunk->position);
        for (size_t section_index = 0; section_index < chunk->meshes->sections.size(); ++section_index) {
            const Chunk_Section_Range& section = chunk->meshes->sections[section_index];
            if (!section.has_geometry || section.liquid_count == 0 ||
                !chunk->section_visible[section_index]) continue;
            draw_mesh_range(gl_res.liquid_vao, section.liquid_start, section.liquid_count);
        }
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
}

void Renderer::draw_sky(const glm::vec3& camera_position,
                       const glm::vec3& camera_right,
                       const glm::vec3& camera_up,
                       const glm::mat4& view,
                       const glm::mat4& projection,
                       const glm::vec3& sun_direction,
                       const glm::vec3& moon_direction) {
    // Классический приём "скайбокса": рисуем без теста/записи глубины, ДО обычной
    // непрозрачной геометрии мира — рельеф естественно перекрывает солнце/луну там, где
    // должен закрывать горизонт, без отдельной логики окклюзии для самих дисков.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    constexpr float distance = 400.0f;
    constexpr float sun_radius = 28.0f;
    constexpr float moon_radius = 20.0f;

    m_sky_shader_ptr->use();
    m_sky_shader_ptr->set_mat4("view", view);
    m_sky_shader_ptr->set_mat4("projection", projection);

    glBindVertexArray(m_sky_quad_vao);

    const auto draw_disc = [&](const glm::vec3& direction, float radius, const glm::vec3& color, float alpha) {
        if (alpha <= 0.0f) return;
        const glm::vec3 center = camera_position + direction * distance;
        m_sky_shader_ptr->set_vec3("center", center);
        m_sky_shader_ptr->set_vec3("right", camera_right * radius);
        m_sky_shader_ptr->set_vec3("up", camera_up * radius);
        m_sky_shader_ptr->set_vec3("color", color);
        m_sky_shader_ptr->set_float("alpha", alpha);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    };

    // Мягкое исчезновение под горизонтом (а не резкое пропадание ровно на y=0) — на глаз
    // выглядит естественнее, особенно с учётом того, что диск не идеально маленький.
    const auto fade = [](float elevation) {
        return glm::clamp((elevation + 0.06f) / 0.12f, 0.0f, 1.0f);
    };

    draw_disc(sun_direction, sun_radius, glm::vec3(1.0f, 0.95f, 0.75f), fade(sun_direction.y));
    draw_disc(moon_direction, moon_radius, glm::vec3(0.85f, 0.87f, 0.92f), fade(moon_direction.y));

    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::invalidate_chunk_cache(const glm::ivec3& chunk_pos) {
    auto it = m_chunk_gl_cache.find(chunk_pos);
    if (it != m_chunk_gl_cache.end()) {
        delete_chunk_resources(it->second);
        m_chunk_gl_cache.erase(it);
    }
}
