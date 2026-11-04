//
// Created by noktemor on 11.02.2026.
//
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include "rendering/Renderer.h"
#include "utils/logging/Logger.h"
#include "utils/Config.h"
#include "rendering/Frustum.h"

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

    // EXT/NV mesh shader stages are not part of the project's GLAD 4.3 build.
    // Khronos assigns the same stage enum values to both extension variants.
    constexpr GLenum kGlTaskShader = 0x955A;
    constexpr GLenum kGlMeshShader = 0x9559;
    constexpr GLenum kGlParameterBufferArb = 0x80EE;
    constexpr GLbitfield kGpuParameterBufferBarrierBit = 0x00008000;

    bool has_gl_extension(const char* wanted) {
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; ++i) {
            const auto* value = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            if (value && std::strcmp(value, wanted) == 0) return true;
        }
        return false;
    }

    std::string read_shader_source(const std::string& path) {
        std::ifstream file(path);
        if (!file) {
            LOG_ERROR("Cannot open shader: " + path);
            return {};
        }
        std::stringstream stream;
        stream << file.rdbuf();
        return stream.str();
    }

    GLuint compile_stage(GLenum stage, const std::string& path) {
        const std::string source = read_shader_source(path);
        if (source.empty()) return 0;
        const char* source_ptr = source.c_str();
        const GLuint shader = glCreateShader(stage);
        glShaderSource(shader, 1, &source_ptr, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[8192]{};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            LOG_ERROR("Shader compile error (" + path + "): " + log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    GLuint compile_mesh_program(const std::string& task_path, const std::string& mesh_path,
                                const std::string& fragment_path) {
        const GLuint task = compile_stage(kGlTaskShader, task_path);
        const GLuint mesh = compile_stage(kGlMeshShader, mesh_path);
        const GLuint fragment = compile_stage(GL_FRAGMENT_SHADER, fragment_path);
        if (!task || !mesh || !fragment) {
            if (task) glDeleteShader(task);
            if (mesh) glDeleteShader(mesh);
            if (fragment) glDeleteShader(fragment);
            return 0;
        }

        const GLuint program = glCreateProgram();
        glAttachShader(program, task);
        glAttachShader(program, mesh);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glDeleteShader(task);
        glDeleteShader(mesh);
        glDeleteShader(fragment);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[8192]{};
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            LOG_ERROR("Mesh-shader program link error (" + mesh_path + "): " + log);
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

Renderer::Renderer() = default;

Renderer::~Renderer() {
    for (auto& [key, res] : m_chunk_gl_cache) {
        delete_chunk_resources(res);
    }
    shutdown_gpu_mesher();

    destroy_scene_targets();
    delete m_post_shader_ptr;
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

bool Renderer::initialize(const std::string& blocks_atlas_name) {
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

    // Постобработка (FXAA, зерно). Полноэкранный квад — тот же vignette_vertex.glsl. Если шейдер не собрался,
    // игра работает без постобработки: мир рисуется прямо в кадр.
    m_post_shader_ptr = new Shader("assets/shaders/vignette_vertex.glsl", "assets/shaders/post_fragment.glsl");
    if (m_post_shader_ptr->get_id() == 0) {
        LOG_ERROR("Failed to create post-processing shader program; FXAA and film grain are disabled");
        delete m_post_shader_ptr;
        m_post_shader_ptr = nullptr;
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

    // Геометрия мира строится только на видеокарте: без GL 4.3 (compute-шейдеры) игра не запускается.
    if (!initialize_gpu_mesher()) {
        LOG_ERROR("The GPU mesher failed to start: OpenGL 4.3 with compute shaders is required");
        return false;
    }

    LOG_INFO("Renderer initialized with " + std::to_string(m_frame_workers.worker_count()) +
             " persistent CPU frame-preparation worker(s)");
    return true;
}

void Renderer::destroy_scene_targets() {
    if (m_scene_fbo) glDeleteFramebuffers(1, &m_scene_fbo);
    if (m_scene_color) glDeleteTextures(1, &m_scene_color);
    if (m_scene_depth) glDeleteRenderbuffers(1, &m_scene_depth);
    m_scene_fbo = m_scene_color = m_scene_depth = 0;
    m_scene_width = m_scene_height = 0;
}

// Пересоздаёт промежуточный буфер при смене размера окна. Привязку кадра не меняет: вызывающий
// сам сохраняет и восстанавливает её.
void Renderer::ensure_scene_targets(int width, int height) {
    if (m_scene_fbo && width == m_scene_width && height == m_scene_height) return;
    destroy_scene_targets();

    GLint previous_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);

    glGenTextures(1, &m_scene_color);
    glBindTexture(GL_TEXTURE_2D, m_scene_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &m_scene_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, m_scene_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    glGenFramebuffers(1, &m_scene_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_scene_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_scene_depth);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_fbo));
    if (!complete) {
        LOG_ERROR("Scene framebuffer is incomplete; post-processing is disabled");
        destroy_scene_targets();
        return;
    }
    m_scene_width = width;
    m_scene_height = height;
}

bool Renderer::begin_scene(int width, int height) {
    if (m_aa_mode == 0 && m_film_grain <= 0.0f) return false;
    if (!m_post_shader_ptr || width <= 0 || height <= 0) return false;
    ensure_scene_targets(width, height);
    if (!m_scene_fbo) return false;

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_saved_framebuffer);
    glGetIntegerv(GL_VIEWPORT, m_saved_viewport);
    glBindFramebuffer(GL_FRAMEBUFFER, m_scene_fbo);
    glViewport(0, 0, width, height);
    m_scene_active = true;
    return true;
}

void Renderer::end_scene_and_post() {
    if (!m_scene_active) return;
    m_scene_active = false;

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(m_saved_framebuffer));
    glViewport(m_saved_viewport[0], m_saved_viewport[1], m_saved_viewport[2], m_saved_viewport[3]);

    // Полноэкранный проход: без глубины, отсечения, наложения и обрезки ножницами.
    const GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    m_post_shader_ptr->use();
    m_post_shader_ptr->set_int("scene", 0);
    m_post_shader_ptr->set_vec2("texel", glm::vec2(1.0f / static_cast<float>(m_scene_width),
                                                   1.0f / static_cast<float>(m_scene_height)));
    m_post_shader_ptr->set_int("useFxaa", m_aa_mode == 1 ? 1 : 0);
    m_post_shader_ptr->set_float("grain", m_film_grain * Config::film_grain_max_amplitude);
    m_post_shader_ptr->set_float("time", static_cast<float>(glfwGetTime()));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_scene_color);
    glBindVertexArray(m_sky_quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (scissor_was_enabled) glEnable(GL_SCISSOR_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::clear_screen(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
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

void Renderer::delete_chunk_resources(Chunk_GL_Resources& resources) {
    release_gpu_chunk(resources);
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
    if (!GLAD_GL_VERSION_4_3) {
        LOG_ERROR("OpenGL 4.3 is not available");
        return false;
    }

    m_gpu_greedy_program = compile_compute_program("assets/shaders/mesh_greedy.comp");
    m_gpu_misc_program = compile_compute_program("assets/shaders/mesh_misc.comp");
    m_gpu_cull_program = compile_compute_program("assets/shaders/gpu_cull_commands.comp");
    // Hi-Z is an optional second-stage optimization; the core GPU frustum/command path
    // remains operational even on drivers where these helper shaders fail to compile.
    m_gpu_hiz_copy_program = compile_compute_program("assets/shaders/gpu_hiz_copy.comp");
    m_gpu_hiz_reduce_program = compile_compute_program("assets/shaders/gpu_hiz_reduce.comp");
    m_gpu_terrain_shader_ptr = new Shader("assets/shaders/terrain_gpu_vertex.glsl",
                                          "assets/shaders/terrain_gpu_fragment.glsl");
    m_gpu_misc_shader_ptr = new Shader("assets/shaders/terrain_gpu_misc_vertex.glsl",
                                       "assets/shaders/terrain_gpu_misc_fragment.glsl");
    // Shader не бросает исключений при ошибке линковки, поэтому статус проверяем явно.
    const auto linked = [](const Shader* shader) {
        GLint ok = GL_FALSE;
        if (shader && shader->get_id() != 0) glGetProgramiv(shader->get_id(), GL_LINK_STATUS, &ok);
        return ok == GL_TRUE;
    };
    if (m_gpu_greedy_program == 0 || m_gpu_misc_program == 0 || m_gpu_cull_program == 0 ||
        !linked(m_gpu_terrain_shader_ptr) || !linked(m_gpu_misc_shader_ptr)) {
        LOG_ERROR("GPU mesher shader setup failed");
        shutdown_gpu_mesher();
        return false;
    }
    m_gpu_loc_greedy_chunk_count = glGetUniformLocation(m_gpu_greedy_program, "u_chunk_count");
    m_gpu_loc_misc_chunk_count = glGetUniformLocation(m_gpu_misc_program, "u_chunk_count");
    m_gpu_loc_cull_candidate_count = glGetUniformLocation(m_gpu_cull_program, "u_candidate_count");
    m_gpu_loc_cull_mesh_mode = glGetUniformLocation(m_gpu_cull_program, "u_mesh_mode");
    m_gpu_loc_cull_planes = glGetUniformLocation(m_gpu_cull_program, "u_frustum_planes[0]");
    m_gpu_loc_cull_view_projection = glGetUniformLocation(m_gpu_cull_program, "u_view_projection");
    m_gpu_loc_cull_viewport_size = glGetUniformLocation(m_gpu_cull_program, "u_viewport_size");
    m_gpu_loc_cull_use_hiz = glGetUniformLocation(m_gpu_cull_program, "u_use_hiz");
    m_gpu_loc_cull_compact_opaque = glGetUniformLocation(m_gpu_cull_program, "u_compact_opaque");
    m_gpu_loc_cull_hiz_sampler = glGetUniformLocation(m_gpu_cull_program, "u_hiz");
    if (m_gpu_hiz_copy_program != 0)
        m_gpu_loc_hiz_copy_sampler = glGetUniformLocation(m_gpu_hiz_copy_program, "u_source_depth");
    if (m_gpu_hiz_reduce_program != 0) {
        m_gpu_loc_hiz_reduce_sampler = glGetUniformLocation(m_gpu_hiz_reduce_program, "u_source_hiz");
        m_gpu_loc_hiz_reduce_source_level = glGetUniformLocation(m_gpu_hiz_reduce_program, "u_source_level");
    }
    if (m_gpu_hiz_copy_program == 0 || m_gpu_hiz_reduce_program == 0)
        LOG_WARN("Hi-Z shaders unavailable; GPU frustum culling remains enabled without temporal occlusion");

    // True mesh shading is optional because neither extension is required by GL 4.3.
    // Prefer the cross-vendor EXT spelling, then try NVIDIA's NV spelling. Both replace
    // only the final vertex-pulling draw stage; compute meshing and the GL 4.3 fallback stay intact.
    const bool draw_id_available = has_gl_extension("GL_ARB_shader_draw_parameters");
    auto try_enable_mesh_pipeline = [&](const char* extension_name,
                                        const char* indirect_entry_point,
                                        const char* task_shader,
                                        const char* solid_mesh_shader,
                                        const char* misc_mesh_shader,
                                        int mode,
                                        const char* label) -> bool {
        if (!has_gl_extension(extension_name) || !draw_id_available) return false;

        auto draw_tasks = reinterpret_cast<MultiDrawMeshTasksIndirect>(
            glfwGetProcAddress(indirect_entry_point));
        if (!draw_tasks) {
            LOG_WARN(std::string(extension_name) + " is advertised, but " + indirect_entry_point +
                     " could not be loaded");
            return false;
        }

        const GLuint solid_program = compile_mesh_program(
            task_shader, solid_mesh_shader, "assets/shaders/terrain_gpu_fragment.glsl");
        const GLuint misc_program = compile_mesh_program(
            task_shader, misc_mesh_shader, "assets/shaders/terrain_gpu_misc_fragment.glsl");
        if (!solid_program || !misc_program) {
            LOG_ERROR(std::string(label) + " mesh-shader setup failed; trying another backend/fallback");
            if (solid_program) glDeleteProgram(solid_program);
            if (misc_program) glDeleteProgram(misc_program);
            return false;
        }

        m_gpu_mesh_solid_program = solid_program;
        m_gpu_mesh_misc_program = misc_program;
        m_gl_multi_draw_mesh_tasks_indirect = draw_tasks;
        m_gpu_mesh_shader_mode = mode;
        m_gpu_mesh_shader_ready = true;
        m_gpu_loc_mesh_order_offset_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "u_order_offset");
        m_gpu_loc_mesh_order_offset_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "u_order_offset");
        m_gpu_loc_mesh_draw_map_offset_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "u_draw_map_offset");
        m_gpu_loc_mesh_draw_map_offset_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "u_draw_map_offset");
        m_gpu_loc_mesh_kind_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "u_kind");
        m_gpu_loc_mesh_kind_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "u_kind");
        m_gpu_loc_mesh_view_projection_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "view_projection");
        m_gpu_loc_mesh_view_projection_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "view_projection");
        m_gpu_loc_mesh_time_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "time");
        m_gpu_loc_mesh_appear_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "appearDuration");
        m_gpu_loc_mesh_appear_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "appearDuration");
        m_gpu_loc_mesh_ambient_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "ambient");
        m_gpu_loc_mesh_ambient_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "ambient");
        m_gpu_loc_mesh_max_light_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "maxLightLevel");
        m_gpu_loc_mesh_max_light_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "maxLightLevel");
        m_gpu_loc_mesh_time_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "time");
        m_gpu_loc_mesh_camera_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "cameraPosition");
        m_gpu_loc_mesh_camera_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "cameraPosition");
        m_gpu_loc_mesh_fog_color_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "fogColor");
        m_gpu_loc_mesh_fog_color_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "fogColor");
        m_gpu_loc_mesh_fog_range_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "fogRange");
        m_gpu_loc_mesh_fog_range_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "fogRange");
        m_gpu_loc_mesh_sun_direction_solid = glGetUniformLocation(m_gpu_mesh_solid_program, "sunDirection");
        m_gpu_loc_mesh_sun_direction_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "sunDirection");
        m_gpu_loc_mesh_tile_origin_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "tile_origin");
        m_gpu_loc_mesh_tile_size_misc = glGetUniformLocation(m_gpu_mesh_misc_program, "tile_size");
        LOG_INFO(std::string("OpenGL ") + label + " task + mesh shader pipeline enabled");
        return true;
    };

    bool mesh_pipeline_enabled = try_enable_mesh_pipeline(
        "GL_EXT_mesh_shader", "glMultiDrawMeshTasksIndirectEXT",
        "assets/shaders/mesh_task.task", "assets/shaders/terrain_mesh.mesh",
        "assets/shaders/terrain_misc_mesh.mesh", 1, "EXT");
    if (!mesh_pipeline_enabled) {
        mesh_pipeline_enabled = try_enable_mesh_pipeline(
            "GL_NV_mesh_shader", "glMultiDrawMeshTasksIndirectNV",
            "assets/shaders/mesh_task_nv.task", "assets/shaders/terrain_mesh_nv.mesh",
            "assets/shaders/terrain_misc_mesh_nv.mesh", 2, "NV");
    }
    if (!mesh_pipeline_enabled) {
        m_gpu_mesh_shader_ready = false;
        m_gpu_mesh_shader_mode = 0;
        m_gl_multi_draw_mesh_tasks_indirect = nullptr;
        LOG_INFO("No usable task/mesh shader extension; using the GL 4.3 indirect vertex-pulling path");
    }

    // ARB_indirect_parameters lets the GPU provide the number of non-empty draw commands.
    // That avoids submitting thousands of zero-count commands at large render distances.
    // Transparent commands are intentionally left on the fixed-count path to preserve
    // their deterministic back-to-front ordering.
    const char* gl_version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    int gl_major = 0, gl_minor = 0;
    if (gl_version) std::sscanf(gl_version, "%d.%d", &gl_major, &gl_minor);
    const bool indirect_parameters_supported = has_gl_extension("GL_ARB_indirect_parameters") ||
        gl_major > 4 || (gl_major == 4 && gl_minor >= 6);
    if (indirect_parameters_supported) {
        m_gl_multi_draw_arrays_indirect_count = reinterpret_cast<MultiDrawArraysIndirectCount>(
            glfwGetProcAddress("glMultiDrawArraysIndirectCountARB"));
        if (m_gpu_mesh_shader_mode == 1) {
            m_gl_multi_draw_mesh_tasks_indirect_count = reinterpret_cast<MultiDrawMeshTasksIndirectCount>(
                glfwGetProcAddress("glMultiDrawMeshTasksIndirectCountEXT"));
        } else if (m_gpu_mesh_shader_mode == 2) {
            m_gl_multi_draw_mesh_tasks_indirect_count = reinterpret_cast<MultiDrawMeshTasksIndirectCount>(
                glfwGetProcAddress("glMultiDrawMeshTasksIndirectCountNV"));
        }
    }
    m_gpu_compact_opaque_commands = m_gpu_mesh_shader_ready
        ? (m_gl_multi_draw_mesh_tasks_indirect_count != nullptr)
        : (m_gl_multi_draw_arrays_indirect_count != nullptr);
    if (m_gpu_compact_opaque_commands) {
        LOG_INFO("GPU indirect-command compaction enabled (opaque draw count stays on GPU)");
    } else {
        LOG_INFO("Indirect-count API unavailable; using fixed-count indirect commands");
    }

    // Таблица UV строится теми же resolve_face_uv/get_uv, по которым когда-то мешил CPU, поэтому
    // картинка не зависит от того, что видеокарта "знает" о блоках сама. LUT растёт с реально
    // зарегистрированными id (а не с потолком 65536): 6 граней x 256 meta на id.
    // Блоки формы, флора и жидкость всегда берут спрайт по get_uv (верх/низ/бок) без учёта метаданных.
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
        const bool misc = props.shape != Block_Shape::None || props.mesh_style == Block_Mesh_Style::XStyle ||
                          props.state == Block_State::Liquid;
        for (int face = 0; face < 6; ++face) {
            for (int meta = 0; meta < 256; ++meta) {
                uint32_t entry = 0;
                if (props.uses_meta && !misc) {
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
        LOG_ERROR("GPU mesher: too many UV rectangles for the 15-bit index");
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
    m_gpu_shape_index_ssbo = create_ssbo(tables.shape_index.size() * sizeof(uint32_t), tables.shape_index.data(), GL_STATIC_DRAW);
    m_gpu_shape_boxes_ssbo = create_ssbo(tables.shape_boxes.size() * sizeof(uint32_t), tables.shape_boxes.data(), GL_STATIC_DRAW);
    m_gpu_rects_ssbo = create_ssbo(rects.size() * sizeof(glm::vec4), rects.data(), GL_STATIC_DRAW);
    m_gpu_headers_ssbo = create_ssbo(static_cast<size_t>(kGpuBatchChunks) * kGpuHeaderStride * sizeof(glm::uvec4), nullptr, GL_STREAM_DRAW);
    m_gpu_payload_capacity = size_t{1} << 20;
    m_gpu_payload_ssbo = create_ssbo(m_gpu_payload_capacity, nullptr, GL_STREAM_DRAW);
    // GPU culling counters are copied into a small asynchronous readback ring. Reading them
    // directly from the live SSBO would serialize the CPU with the GPU every frame.
    m_gpu_cull_stats_ssbo = create_ssbo(5u * sizeof(uint32_t), nullptr, GL_DYNAMIC_DRAW);
    for (Gpu_Cull_Stats_Readback& readback : m_gpu_cull_stats_readbacks) {
        readback.buffer = create_ssbo(5u * sizeof(uint32_t), nullptr, GL_STREAM_READ);
    }
    for (Gpu_Readback& rb : m_gpu_readbacks) {
        rb.buffer = create_ssbo(static_cast<size_t>(kGpuBatchChunks) * kGpuStreams * sizeof(uint32_t), nullptr, GL_STREAM_READ);
    }
    glGenVertexArrays(1, &m_gpu_section_vao);
    m_gpu_pool.total_blocks = 0;
    m_gpu_pool.free_runs.clear();
    grow_gpu_pool(kGpuInitialPoolBlocks);
    grow_gpu_handles(256);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    m_gpu_ready = true;
    const char* mesh_backend = m_gpu_mesh_shader_mode == 1 ? "EXT task/mesh shading" :
                               (m_gpu_mesh_shader_mode == 2 ? "NV task/mesh shading" :
                                                              "vertex-pulling fallback");
    LOG_INFO(std::string("GPU mesher ready (compute + ") + mesh_backend +
             (m_gpu_hiz_copy_program && m_gpu_hiz_reduce_program
                  ? ", GPU frustum/Hi-Z culling, unified quad pool)"
                  : ", GPU frustum culling, unified quad pool)"));
    return true;
}

void Renderer::shutdown_gpu_mesher() {
    for (Gpu_Readback& rb : m_gpu_readbacks) {
        if (rb.fence) glDeleteSync(rb.fence);
        if (rb.buffer) glDeleteBuffers(1, &rb.buffer);
        rb = Gpu_Readback{};
    }
    for (Gpu_Cull_Stats_Readback& rb : m_gpu_cull_stats_readbacks) {
        if (rb.fence) glDeleteSync(rb.fence);
        if (rb.buffer) glDeleteBuffers(1, &rb.buffer);
        rb = Gpu_Cull_Stats_Readback{};
    }
    for (Gpu_Frame_Timer_Query& timer : m_gpu_frame_timer_queries) {
        if (timer.id) glDeleteQueries(1, &timer.id);
        timer = Gpu_Frame_Timer_Query{};
    }
    m_gpu_frame_timer_write = 0;
    m_last_gpu_frame_ms = 0.0f;
    unsigned int buffers[] = {
        m_gpu_lut_ssbo, m_gpu_tables_ssbo, m_gpu_shape_index_ssbo, m_gpu_shape_boxes_ssbo, m_gpu_rects_ssbo,
        m_gpu_headers_ssbo, m_gpu_payload_ssbo, m_gpu_quads_ssbo, m_gpu_counts_ssbo, m_gpu_ranges_ssbo,
        m_gpu_origins_ssbo, m_gpu_candidates_ssbo, m_gpu_section_bounds_ssbo, m_gpu_order_ssbo,
        m_gpu_cmd_ssbo, m_gpu_cmd_meta_ssbo, m_gpu_cull_stats_ssbo, m_gpu_section_ids_vbo
    };
    for (unsigned int id : buffers) {
        if (id != 0) glDeleteBuffers(1, &id);
    }
    if (m_gpu_section_vao != 0) glDeleteVertexArrays(1, &m_gpu_section_vao);
    if (m_gpu_greedy_program != 0) glDeleteProgram(m_gpu_greedy_program);
    if (m_gpu_misc_program != 0) glDeleteProgram(m_gpu_misc_program);
    if (m_gpu_cull_program != 0) glDeleteProgram(m_gpu_cull_program);
    if (m_gpu_hiz_copy_program != 0) glDeleteProgram(m_gpu_hiz_copy_program);
    if (m_gpu_hiz_reduce_program != 0) glDeleteProgram(m_gpu_hiz_reduce_program);
    if (m_gpu_hiz_depth_texture != 0) glDeleteTextures(1, &m_gpu_hiz_depth_texture);
    if (m_gpu_hiz_texture != 0) glDeleteTextures(1, &m_gpu_hiz_texture);
    if (m_gpu_hiz_fbo != 0) glDeleteFramebuffers(1, &m_gpu_hiz_fbo);
    if (m_gpu_mesh_solid_program != 0) glDeleteProgram(m_gpu_mesh_solid_program);
    if (m_gpu_mesh_misc_program != 0) glDeleteProgram(m_gpu_mesh_misc_program);
    delete m_gpu_terrain_shader_ptr;
    delete m_gpu_misc_shader_ptr;

    m_gpu_terrain_shader_ptr = nullptr;
    m_gpu_misc_shader_ptr = nullptr;
    m_gpu_greedy_program = m_gpu_misc_program = m_gpu_cull_program = 0;
    m_gpu_hiz_copy_program = m_gpu_hiz_reduce_program = 0;
    m_gpu_hiz_depth_texture = m_gpu_hiz_texture = m_gpu_hiz_fbo = 0;
    m_gpu_hiz_width = m_gpu_hiz_height = m_gpu_hiz_levels = 0;
    m_gpu_hiz_viewport_x = m_gpu_hiz_viewport_y = 0;
    m_gpu_hiz_viewport_width = m_gpu_hiz_viewport_height = 0;
    m_gpu_hiz_valid = false;
    m_gpu_hiz_scene_signature = m_gpu_hiz_pending_signature = 0;
    m_gpu_hiz_view_projection = glm::mat4(1.0f);
    m_gpu_hiz_candidate_view_projection = glm::mat4(1.0f);
    m_gpu_hiz_candidate_signature = 0;
    m_gpu_hiz_candidate_viewport[0] = m_gpu_hiz_candidate_viewport[1] = 0;
    m_gpu_hiz_candidate_viewport[2] = m_gpu_hiz_candidate_viewport[3] = 0;
    m_gpu_hiz_stable_frames = 0;
    m_gpu_hiz_candidate_valid = false;
    m_gpu_mesh_solid_program = m_gpu_mesh_misc_program = 0;
    m_gpu_mesh_shader_ready = false;
    m_gpu_mesh_shader_mode = 0;
    m_gl_multi_draw_mesh_tasks_indirect = nullptr;
    m_gl_multi_draw_mesh_tasks_indirect_count = nullptr;
    m_gl_multi_draw_arrays_indirect_count = nullptr;
    m_gpu_compact_opaque_commands = false;
    m_gpu_section_vao = 0;
    m_gpu_section_ids_vbo = 0;
    m_gpu_lut_ssbo = m_gpu_tables_ssbo = m_gpu_shape_index_ssbo = m_gpu_shape_boxes_ssbo = m_gpu_rects_ssbo = 0;
    m_gpu_headers_ssbo = m_gpu_payload_ssbo = 0;
    m_gpu_quads_ssbo = m_gpu_counts_ssbo = m_gpu_ranges_ssbo = m_gpu_origins_ssbo = 0;
    m_gpu_candidates_ssbo = m_gpu_section_bounds_ssbo = 0;
    m_gpu_order_ssbo = m_gpu_cmd_ssbo = m_gpu_cmd_meta_ssbo = m_gpu_cull_stats_ssbo = 0;
    m_gpu_cmd_meta_capacity_bytes = 0;
    m_gpu_payload_capacity = 0;
    m_gpu_candidates_capacity = m_gpu_bounds_capacity = 0;
    m_gpu_order_capacity = m_gpu_cmd_capacity = 0;
    m_gpu_cull_stats_write = 0;
    m_gpu_handle_capacity = 0;
    m_gpu_handle_high_water = 0;
    m_gpu_free_handles.clear();
    m_gpu_pool = Gpu_Pool_Allocator{};
    m_gpu_stream_counts.clear();
    m_gpu_headers_cpu.clear();
    m_gpu_payload_cpu.clear();
    m_gpu_order_cpu.clear();
    m_gpu_readback_head = 0;
    m_gpu_readback_count = 0;
    m_gpu_ready = false;
}

// Пул квадов растёт без потери содержимого: новый буфер получает копию старого, а добавленные блоки
// уходят в список свободных (сливаясь с последним свободным участком, если он упирался в конец).
void Renderer::grow_gpu_pool(uint32_t extra_blocks) {
    const uint32_t old_total = m_gpu_pool.total_blocks;
    const uint32_t new_total = old_total + std::max(extra_blocks, old_total / 2);
    const size_t block_bytes = static_cast<size_t>(kGpuBlockQuads) * sizeof(glm::uvec4);
    const unsigned int quads = create_ssbo(static_cast<size_t>(new_total) * block_bytes, nullptr, GL_DYNAMIC_DRAW);
    if (m_gpu_quads_ssbo != 0) {
        glBindBuffer(GL_COPY_READ_BUFFER, m_gpu_quads_ssbo);
        glBindBuffer(GL_COPY_WRITE_BUFFER, quads);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
                            static_cast<GLsizeiptr>(static_cast<size_t>(old_total) * block_bytes));
        glDeleteBuffers(1, &m_gpu_quads_ssbo);
    }
    m_gpu_quads_ssbo = quads;

    // Добавленный хвост сливается с последним свободным участком, если тот доходил до старого конца.
    uint32_t start = old_total;
    uint32_t length = new_total - old_total;
    if (!m_gpu_pool.free_runs.empty()) {
        const auto last = std::prev(m_gpu_pool.free_runs.end());
        if (last->first + last->second == old_total) {
            start = last->first;
            length += last->second;
            m_gpu_pool.free_runs.erase(last);
        }
    }
    m_gpu_pool.free_runs[start] = length;
    m_gpu_pool.total_blocks = new_total;
}

// Первый квад непрерывного участка на `quads` квадов (с округлением вверх до блока); пул растёт, если места нет.
uint32_t Renderer::allocate_quads(uint32_t quads) {
    const uint32_t blocks = (quads + kGpuBlockQuads - 1) / kGpuBlockQuads;
    for (;;) {
        for (auto it = m_gpu_pool.free_runs.begin(); it != m_gpu_pool.free_runs.end(); ++it) {
            if (it->second < blocks) continue;
            const uint32_t start = it->first;
            const uint32_t length = it->second;
            m_gpu_pool.free_runs.erase(it);
            if (length > blocks) m_gpu_pool.free_runs[start + blocks] = length - blocks;
            return start * kGpuBlockQuads;
        }
        grow_gpu_pool(std::max(blocks, kGpuInitialPoolBlocks));
    }
}

void Renderer::free_quads(uint32_t first, uint32_t quads) {
    if (quads == 0) return;
    uint32_t start = first / kGpuBlockQuads;
    uint32_t length = (quads + kGpuBlockQuads - 1) / kGpuBlockQuads;
    auto next = m_gpu_pool.free_runs.lower_bound(start);
    if (next != m_gpu_pool.free_runs.begin()) {
        const auto prev = std::prev(next);
        if (prev->first + prev->second == start) {
            start = prev->first;
            length += prev->second;
            m_gpu_pool.free_runs.erase(prev);
        }
    }
    if (next != m_gpu_pool.free_runs.end() && start + length == next->first) {
        length += next->second;
        m_gpu_pool.free_runs.erase(next);
    }
    m_gpu_pool.free_runs[start] = length;
}

// Таблицы по ручкам чанка: счётчики, диапазоны потоков, мировые углы. Растут вдвое с переносом содержимого.
void Renderer::grow_gpu_handles(int needed) {
    if (needed <= m_gpu_handle_capacity) return;
    const int new_capacity = std::max(needed, std::max(256, m_gpu_handle_capacity * 2));
    const size_t capacity = static_cast<size_t>(new_capacity);
    const size_t old_capacity = static_cast<size_t>(m_gpu_handle_capacity);

    const std::vector<uint32_t> zero_counts(kGpuStreams * capacity, 0u);
    const std::vector<glm::uvec2> zero_ranges(kGpuStreams * capacity, glm::uvec2(0u));
    const std::vector<glm::vec4> zero_origins(capacity, glm::vec4(0.0f));
    const unsigned int counts = create_ssbo(zero_counts.size() * sizeof(uint32_t), zero_counts.data(), GL_DYNAMIC_DRAW);
    const unsigned int ranges = create_ssbo(zero_ranges.size() * sizeof(glm::uvec2), zero_ranges.data(), GL_DYNAMIC_DRAW);
    const unsigned int origins = create_ssbo(zero_origins.size() * sizeof(glm::vec4), zero_origins.data(), GL_DYNAMIC_DRAW);
    // Тождественные номера для атрибута aSection (baseInstance = ручка*4 + секция).
    std::vector<uint32_t> section_ids(static_cast<size_t>(Config::chunk_section_count) * capacity);
    std::iota(section_ids.begin(), section_ids.end(), 0u);
    unsigned int ids = 0;
    glGenBuffers(1, &ids);
    glBindBuffer(GL_ARRAY_BUFFER, ids);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(section_ids.size() * sizeof(uint32_t)),
                 section_ids.data(), GL_STATIC_DRAW);

    if (old_capacity > 0) {
        const auto copy_bytes = [](unsigned int src, unsigned int dst, size_t bytes) {
            glBindBuffer(GL_COPY_READ_BUFFER, src);
            glBindBuffer(GL_COPY_WRITE_BUFFER, dst);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, static_cast<GLsizeiptr>(bytes));
        };
        copy_bytes(m_gpu_counts_ssbo, counts, kGpuStreams * old_capacity * sizeof(uint32_t));
        copy_bytes(m_gpu_ranges_ssbo, ranges, kGpuStreams * old_capacity * sizeof(glm::uvec2));
        copy_bytes(m_gpu_origins_ssbo, origins, old_capacity * sizeof(glm::vec4));
    }

    unsigned int old_buffers[] = {m_gpu_counts_ssbo, m_gpu_ranges_ssbo, m_gpu_origins_ssbo, m_gpu_section_ids_vbo};
    for (unsigned int old_id : old_buffers) {
        if (old_id != 0) glDeleteBuffers(1, &old_id);
    }
    m_gpu_counts_ssbo = counts;
    m_gpu_ranges_ssbo = ranges;
    m_gpu_origins_ssbo = origins;
    m_gpu_section_ids_vbo = ids;
    glBindVertexArray(m_gpu_section_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_gpu_section_ids_vbo);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(uint32_t), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribDivisor(0, 1);
    glBindVertexArray(0);
    m_gpu_handle_capacity = new_capacity;
    m_gpu_stream_counts.resize(kGpuStreams * capacity, 0u);
}

int Renderer::acquire_gpu_handle() {
    if (!m_gpu_free_handles.empty()) {
        const int handle = m_gpu_free_handles.back();
        m_gpu_free_handles.pop_back();
        return handle;
    }
    const int handle = m_gpu_handle_high_water;
    grow_gpu_handles(handle + 1);
    ++m_gpu_handle_high_water;
    return handle;
}

// Чанк уходит с GPU: его участки возвращаются в пул, ручка — в список свободных. Диапазоны на видеокарте
// обнуляются, чтобы устаревшая запись списка отрисовки никогда не читала чужую память.
void Renderer::release_gpu_chunk(Chunk_GL_Resources& resources) {
    if (resources.gpu_handle < 0) return;
    for (int j = 0; j < kGpuStreams; ++j) {
        if (resources.gpu_cap[j] != 0) free_quads(resources.gpu_first[j], resources.gpu_cap[j]);
    }
    const size_t base = static_cast<size_t>(resources.gpu_handle) * kGpuStreams;
    const std::array<glm::uvec2, kGpuStreams> zero_ranges{};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_ranges_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(base * sizeof(glm::uvec2)),
                    sizeof(zero_ranges), zero_ranges.data());
    std::fill_n(m_gpu_stream_counts.begin() + static_cast<std::ptrdiff_t>(base), kGpuStreams, 0u);
    m_gpu_free_handles.push_back(resources.gpu_handle);
    resources.gpu_handle = -1;
    resources.gpu_version = 0;
    resources.appearance_start_time = 0.0f;
    resources.gpu_first.fill(0);
    resources.gpu_cap.fill(0);
    resources.gpu_wanted.fill(0);
    resources.gpu_regrow = false;
    resources.gpu_capacity_observed.fill(false);
    resources.gpu_flora_skipped = false;
}

// Забирает счётчики квадов, посчитанные мешерами, и решает, хватает ли потокам ёмкости. Счётчик всегда
// настоящий (квады сверх ёмкости не пишутся, но считаются), поэтому новая ёмкость выбирается точно: чанк
// с переполненным потоком мешится ещё раз и больше не переполняется. Сильно раздутые потоки сжимаются так же.
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
            GL_COPY_READ_BUFFER, 0, static_cast<GLsizeiptr>(count * kGpuStreams * sizeof(uint32_t)), GL_MAP_READ_BIT));
        if (words) {
            for (size_t i = 0; i < count; ++i) {
                const int handle = rb.handles[i];
                const auto it = m_chunk_gl_cache.find(rb.positions[i]);
                // Отчёт относится к квадам именно этой версии под этой ручкой; иначе он устарел.
                if (it == m_chunk_gl_cache.end() || it->second.gpu_handle != handle ||
                    it->second.gpu_version != rb.versions[i]) continue;
                Chunk_GL_Resources& res = it->second;
                bool regrow = false;
                for (int j = 0; j < kGpuStreams; ++j) {
                    const uint32_t quads = words[i * kGpuStreams + j];
                    m_gpu_stream_counts[static_cast<size_t>(handle) * kGpuStreams + j] = quads;
                    const uint32_t cap = res.gpu_cap[j];
                    const uint32_t padded = quads + quads / 4 + kGpuBlockQuads;
                    const uint32_t target = (padded + kGpuBlockQuads - 1) / kGpuBlockQuads * kGpuBlockQuads;
                    if (quads > cap) {
                        res.gpu_wanted[j] = target;
                        regrow = true;
                    } else if (res.gpu_capacity_observed[j] &&
                               cap >= 4 * std::max(target, 64u) && cap - target >= 512) {
                        // Shrink only after this stream has survived at least one
                        // successful sizing pass. Otherwise a deliberately generous
                        // first allocation would immediately trigger a needless remesh.
                        res.gpu_wanted[j] = target;
                        regrow = true;
                    }
                    if (cap != 0) res.gpu_capacity_observed[j] = true;
                }
                if (regrow) res.gpu_regrow = true;
            }
            glUnmapBuffer(GL_COPY_READ_BUFFER);
        }

        glDeleteSync(rb.fence);
        rb.fence = nullptr;
        rb.positions.clear();
        rb.versions.clear();
        rb.handles.clear();
        m_gpu_readback_head = (m_gpu_readback_head + 1) % kGpuReadbackRing;
        --m_gpu_readback_count;
    }
}

void Renderer::submit_gpu_batch(const std::vector<const Renderable_Chunk*>& batch) {
    const size_t n = batch.size();
    m_gpu_headers_cpu.assign(n * kGpuHeaderStride, glm::uvec4(0u));
    m_gpu_payload_cpu.clear();
    Gpu_Readback& rb = m_gpu_readbacks[(m_gpu_readback_head + m_gpu_readback_count) % kGpuReadbackRing];

    struct Chunk_Upload {
        size_t handle;
        glm::vec4 origin;
        std::array<glm::uvec2, kGpuStreams> ranges;
    };
    std::vector<Chunk_Upload> uploads;
    uploads.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        const Renderable_Chunk& chunk = *batch[i];
        const Chunk_Meshes& meshes = *chunk.meshes;
        Chunk_GL_Resources& res = m_chunk_gl_cache[chunk.position];
        if (res.gpu_handle < 0) {
            res.gpu_handle = acquire_gpu_handle();
            res.gpu_first.fill(0);
            res.gpu_cap.fill(0);
            // Начинаем анимацию только при первой GPU-резиденции. Пересборка меша
            // (например, после правки блока или смены LOD) не должна перезапускать эффект.
            res.appearance_start_time = static_cast<float>(glfwGetTime());
        }
        res.gpu_version = chunk.mesh_version;
        res.gpu_flora_skipped = chunk.flora_skip_render;
        const size_t handle = static_cast<size_t>(res.gpu_handle);

        // Ёмкости потоков: только там, где секция может дать геометрию этого вида. Размер, подобранный по
        // прошлому readback'у (gpu_wanted) или оставшийся с прошлой версии меша, сохраняется; новому потоку
        // даётся стартовая ёмкость вида.
        std::array<glm::uvec2, kGpuStreams> ranges{};
        for (int k = 0; k < kGpuKindCount; ++k) {
            for (int s = 0; s < Config::chunk_section_count; ++s) {
                const int j = k * Config::chunk_section_count + s;
                uint32_t target = 0;
                const bool stream_needed = ((meshes.sections[s].kinds >> k) & 1u) != 0u &&
                    !(k == kKindFlora && chunk.flora_skip_render);
                if (stream_needed) {
                    target = res.gpu_wanted[j] != 0 ? res.gpu_wanted[j]
                           : res.gpu_cap[j] != 0 ? res.gpu_cap[j]
                           : kGpuInitialQuads[k];
                    target = (target + kGpuBlockQuads - 1) / kGpuBlockQuads * kGpuBlockQuads;
                }
                if (target != res.gpu_cap[j]) {
                    const bool was_unallocated = res.gpu_cap[j] == 0;
                    if (res.gpu_cap[j] != 0) free_quads(res.gpu_first[j], res.gpu_cap[j]);
                    res.gpu_cap[j] = target;
                    res.gpu_first[j] = target != 0 ? allocate_quads(target) : 0;
                    if (was_unallocated || target == 0) res.gpu_capacity_observed[j] = false;
                }
                ranges[j] = glm::uvec2(res.gpu_first[j], res.gpu_cap[j]);
            }
        }
        res.gpu_wanted.fill(0);
        res.gpu_regrow = false;

        // Палитры и слова чанка кладём в общий батч; смещения переносим на его начало.
        const uint32_t base = static_cast<uint32_t>(m_gpu_payload_cpu.size());
        m_gpu_payload_cpu.insert(m_gpu_payload_cpu.end(), meshes.gpu_words.begin(), meshes.gpu_words.end());

        // Заголовок чанка: [0].x — ручка, [1] — (флаги, маска видов), [2 + канал] — палитра, слова, битность.
        glm::uvec4* header = &m_gpu_headers_cpu[i * kGpuHeaderStride];
        header[0] = glm::uvec4(static_cast<uint32_t>(res.gpu_handle), 0u, 0u, 0u);
        uint32_t gpu_kinds = static_cast<uint32_t>(meshes.gpu_kinds);
        if (chunk.flora_skip_render) gpu_kinds &= ~(1u << kKindFlora);
        header[1] = glm::uvec4(meshes.positive_faces_only ? 1u : 0u, gpu_kinds,
                               static_cast<uint32_t>(meshes.lod_stride),
                               chunk.flora_skip_render ? 1u : 0u);
        for (int ch = 0; ch < kGpuMeshChannelCount; ++ch) {
            const std::array<uint32_t, 3>& c = meshes.gpu_channels[ch];
            header[2 + ch] = glm::uvec4(base + c[0], base + c[1], c[2], 0u);
        }

        // Мировой угол чанка (w содержит время появления), диапазоны потоков и обнуление счётчиков
        // копятся в списке и уходят в GPU
        // после цикла одним вызовом на непрерывный участок ручек (раньше — 3 glBufferSubData на чанк).
        const glm::vec4 origin(static_cast<float>(chunk.position.x * Config::chunk_size),
                               static_cast<float>(chunk.position.y * Config::chunk_height),
                               static_cast<float>(chunk.position.z * Config::chunk_size),
                               res.appearance_start_time);
        uploads.push_back({handle, origin, ranges});
        std::fill_n(m_gpu_stream_counts.begin() + static_cast<std::ptrdiff_t>(handle * kGpuStreams), kGpuStreams, 0u);

        rb.positions.push_back(chunk.position);
        rb.versions.push_back(chunk.mesh_version);
        rb.handles.push_back(res.gpu_handle);
    }

    // Непрерывные участки ручек: origin / диапазоны / нули счётчиков — по одному вызову на участок.
    std::stable_sort(uploads.begin(), uploads.end(),
              [](const Chunk_Upload& a, const Chunk_Upload& b) { return a.handle < b.handle; });
    {
        std::vector<glm::vec4> run_origins;
        std::vector<glm::uvec2> run_ranges;
        std::vector<uint32_t> run_zero;
        for (size_t a = 0; a < uploads.size();) {
            size_t b = a + 1;
            while (b < uploads.size() && uploads[b].handle == uploads[b - 1].handle + 1) ++b;
            const size_t len = b - a;
            run_origins.resize(len);
            run_ranges.resize(len * kGpuStreams);
            run_zero.assign(len * kGpuStreams, 0u);
            for (size_t r = 0; r < len; ++r) {
                run_origins[r] = uploads[a + r].origin;
                std::copy(uploads[a + r].ranges.begin(), uploads[a + r].ranges.end(),
                          run_ranges.begin() + static_cast<std::ptrdiff_t>(r * kGpuStreams));
            }
            const size_t handle0 = uploads[a].handle;
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_origins_ssbo);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(handle0 * sizeof(glm::vec4)),
                            static_cast<GLsizeiptr>(len * sizeof(glm::vec4)), run_origins.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_ranges_ssbo);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(handle0 * kGpuStreams * sizeof(glm::uvec2)),
                            static_cast<GLsizeiptr>(len * kGpuStreams * sizeof(glm::uvec2)), run_ranges.data());
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_counts_ssbo);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(handle0 * kGpuStreams * sizeof(uint32_t)),
                            static_cast<GLsizeiptr>(len * kGpuStreams * sizeof(uint32_t)), run_zero.data());
            a = b;
        }
    }

    // Header/payload используются только текущим compute batch. Orphaning даёт драйверу
    // новый backing store вместо ожидания, пока GPU закончит читать предыдущий кадр.
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_headers_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(static_cast<size_t>(kGpuBatchChunks) * kGpuHeaderStride * sizeof(glm::uvec4)),
                 nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(n * kGpuHeaderStride * sizeof(glm::uvec4)),
                    m_gpu_headers_cpu.data());
    const size_t payload_bytes = m_gpu_payload_cpu.size() * sizeof(uint32_t);
    if (payload_bytes > m_gpu_payload_capacity) {
        m_gpu_payload_capacity = std::max(payload_bytes * 2, m_gpu_payload_capacity * 2);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_payload_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(m_gpu_payload_capacity), nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(payload_bytes), m_gpu_payload_cpu.data());

    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_gpu_headers_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_gpu_payload_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_gpu_quads_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_gpu_counts_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_gpu_lut_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_gpu_tables_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_gpu_ranges_ssbo);
    // Кубы: 576 работ на чанк (72 среза x 2 знака x 4 секции), по 64 в группе = 9 групп на чанк.
    glUseProgram(m_gpu_greedy_program);
    glUniform1i(m_gpu_loc_greedy_chunk_count, static_cast<GLint>(n));
    glDispatchCompute(static_cast<GLuint>(n) * 9u, 1, 1);
    // Формы, флора, жидкость: по строке клеток (слой y, ряд z) на инвокацию — 1024 работы на чанк, 16 групп.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_gpu_shape_index_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, m_gpu_shape_boxes_ssbo);
    glUseProgram(m_gpu_misc_program);
    glUniform1i(m_gpu_loc_misc_chunk_count, static_cast<GLint>(n));
    glDispatchCompute(static_cast<GLuint>(n) * 16u, 1, 1);

    // Счётчики квадов уходят в readback: по ним подгоняется ёмкость потоков, не останавливая конвейер.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_COPY_READ_BUFFER, m_gpu_counts_ssbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, rb.buffer);
    // Подряд идущие ручки копируются одним вызовом.
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        while (j < n && rb.handles[j] == rb.handles[j - 1] + 1) ++j;
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                            static_cast<GLintptr>(static_cast<size_t>(rb.handles[i]) * kGpuStreams * sizeof(uint32_t)),
                            static_cast<GLintptr>(i * kGpuStreams * sizeof(uint32_t)),
                            static_cast<GLsizeiptr>((j - i) * kGpuStreams * sizeof(uint32_t)));
        i = j;
    }
    rb.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    ++m_gpu_readback_count;
}

bool Renderer::debug_gpu_section_quads(const glm::ivec3& chunk_position, int section,
                                       std::vector<glm::uvec4>& out, int kind) {
    out.clear();
    const auto it = m_chunk_gl_cache.find(chunk_position);
    if (!m_gpu_ready || it == m_chunk_gl_cache.end() || it->second.gpu_handle < 0) return false;
    const Chunk_GL_Resources& res = it->second;
    const int j = kind * Config::chunk_section_count + section;
    const size_t counter = static_cast<size_t>(res.gpu_handle) * kGpuStreams + static_cast<size_t>(j);
    const size_t count = std::min(static_cast<size_t>(m_gpu_stream_counts[counter]),
                                  static_cast<size_t>(res.gpu_cap[j]));
    out.resize(count);
    if (count > 0) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_quads_ssbo);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,
                           static_cast<GLintptr>(static_cast<size_t>(res.gpu_first[j]) * sizeof(glm::uvec4)),
                           static_cast<GLsizeiptr>(count * sizeof(glm::uvec4)), out.data());
    }
    return true;
}

// Подготовка кадра: загружает обновлённые GPU-меши по бюджету, после чего отправляет на GPU
// только кандидатов и метаданные секций. GPU выполняет frustum/Hi-Z culling и пишет indirect-команды.
void Renderer::build_gpu_hiz(const glm::mat4& view_projection) {
    if (m_gpu_hiz_copy_program == 0 || m_gpu_hiz_reduce_program == 0) {
        m_gpu_hiz_valid = false;
        return;
    }

    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) {
        m_gpu_hiz_valid = false;
        m_gpu_hiz_candidate_valid = false;
        m_gpu_hiz_stable_frames = 0;
        return;
    }

    // A temporal Hi-Z pyramid is useful only when it can be reused by the next frame.
    // Rebuilding a full-resolution pyramid on every camera turn wastes several compute
    // dispatches while prepare_gpu_frame deliberately rejects that moving-camera pyramid.
    // Require two consecutive stable frames; during motion, keep only the inexpensive
    // GPU frustum culling path. Once a pyramid matches the current view and scene, reuse
    // it instead of copying/reducing the depth buffer again every frame.
    bool candidate_matches = m_gpu_hiz_candidate_valid &&
        m_gpu_hiz_candidate_signature == m_gpu_hiz_pending_signature;
    for (int i = 0; i < 4 && candidate_matches; ++i)
        candidate_matches = m_gpu_hiz_candidate_viewport[i] == viewport[i];
    if (candidate_matches) {
        for (int column = 0; column < 4 && candidate_matches; ++column) {
            for (int row = 0; row < 4; ++row) {
                if (std::abs(view_projection[column][row] -
                             m_gpu_hiz_candidate_view_projection[column][row]) > 1.0e-6f) {
                    candidate_matches = false;
                    break;
                }
            }
        }
    }
    if (!candidate_matches) {
        m_gpu_hiz_candidate_valid = true;
        m_gpu_hiz_candidate_signature = m_gpu_hiz_pending_signature;
        for (int i = 0; i < 4; ++i) m_gpu_hiz_candidate_viewport[i] = viewport[i];
        m_gpu_hiz_candidate_view_projection = view_projection;
        m_gpu_hiz_stable_frames = 0;
        m_gpu_hiz_valid = false;
        return;
    }
    m_gpu_hiz_stable_frames = std::min(m_gpu_hiz_stable_frames + 1, 2);
    if (m_gpu_hiz_stable_frames < 2) {
        m_gpu_hiz_valid = false;
        return;
    }

    bool already_built_for_candidate = m_gpu_hiz_valid &&
        m_gpu_hiz_scene_signature == m_gpu_hiz_pending_signature &&
        m_gpu_hiz_viewport_x == viewport[0] && m_gpu_hiz_viewport_y == viewport[1] &&
        m_gpu_hiz_viewport_width == viewport[2] && m_gpu_hiz_viewport_height == viewport[3];
    for (int column = 0; column < 4 && already_built_for_candidate; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::abs(view_projection[column][row] - m_gpu_hiz_view_projection[column][row]) > 1.0e-6f) {
                already_built_for_candidate = false;
                break;
            }
        }
    }
    if (already_built_for_candidate) return;

    const auto next_power_of_two = [](int value) {
        unsigned int result = 1u;
        while (result < static_cast<unsigned int>(value)) result <<= 1u;
        return static_cast<int>(result);
    };
    const int width = next_power_of_two(viewport[2]);
    const int height = next_power_of_two(viewport[3]);
    int levels = 1;
    for (int w = width, h = height; w > 1 || h > 1; ++levels) {
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }

    GLint original_read_fbo = 0;
    GLint original_draw_fbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &original_read_fbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &original_draw_fbo);

    // Resize lazily. Power-of-two padding is initialized to depth 1.0, so samples
    // outside the real viewport can never become false occluders in the pyramid.
    if (width != m_gpu_hiz_width || height != m_gpu_hiz_height || levels != m_gpu_hiz_levels ||
        m_gpu_hiz_depth_texture == 0 || m_gpu_hiz_texture == 0 || m_gpu_hiz_fbo == 0) {
        if (m_gpu_hiz_depth_texture) glDeleteTextures(1, &m_gpu_hiz_depth_texture);
        if (m_gpu_hiz_texture) glDeleteTextures(1, &m_gpu_hiz_texture);
        if (m_gpu_hiz_fbo) glDeleteFramebuffers(1, &m_gpu_hiz_fbo);
        m_gpu_hiz_depth_texture = m_gpu_hiz_texture = m_gpu_hiz_fbo = 0;

        glGenTextures(1, &m_gpu_hiz_depth_texture);
        glBindTexture(GL_TEXTURE_2D, m_gpu_hiz_depth_texture);
        // The game's default framebuffer and the isometric-capture FBO both use
        // 24-bit depth. Depth blits require matching source/destination depth formats.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

        glGenFramebuffers(1, &m_gpu_hiz_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_gpu_hiz_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                                m_gpu_hiz_depth_texture, 0);
        // This FBO is used only as a depth blit destination. Do not touch the read
        // buffer selection of the application's currently bound default framebuffer.
        glDrawBuffer(GL_NONE);
        const GLenum framebuffer_status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
            LOG_WARN("Could not create Hi-Z depth framebuffer; temporal occlusion disabled");
            glDeleteFramebuffers(1, &m_gpu_hiz_fbo);
            glDeleteTextures(1, &m_gpu_hiz_depth_texture);
            m_gpu_hiz_fbo = m_gpu_hiz_depth_texture = 0;
            m_gpu_hiz_width = m_gpu_hiz_height = m_gpu_hiz_levels = 0;
            m_gpu_hiz_viewport_x = m_gpu_hiz_viewport_y = 0;
            m_gpu_hiz_viewport_width = m_gpu_hiz_viewport_height = 0;
            m_gpu_hiz_valid = false;
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(original_draw_fbo));
            return;
        }

        glGenTextures(1, &m_gpu_hiz_texture);
        glBindTexture(GL_TEXTURE_2D, m_gpu_hiz_texture);
        int mip_width = width;
        int mip_height = height;
        for (int level = 0; level < levels; ++level) {
            glTexImage2D(GL_TEXTURE_2D, level, GL_R32F, mip_width, mip_height, 0,
                         GL_RED, GL_FLOAT, nullptr);
            mip_width = std::max(1, mip_width / 2);
            mip_height = std::max(1, mip_height / 2);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);

        m_gpu_hiz_width = width;
        m_gpu_hiz_height = height;
        m_gpu_hiz_levels = levels;
        m_gpu_hiz_valid = false;
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_gpu_hiz_fbo);
    // A stale application scissor or a disabled depth write mask must not leave
    // uninitialized pixels in the padded depth target. Preserve both states exactly.
    const GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean original_depth_mask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &original_depth_mask);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    const GLfloat far_depth = 1.0f;
    glClearBufferfv(GL_DEPTH, 0, &far_depth);
    glDepthMask(original_depth_mask);
    if (scissor_was_enabled) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(original_read_fbo));
    glBlitFramebuffer(viewport[0], viewport[1], viewport[0] + viewport[2], viewport[1] + viewport[3],
                      0, 0, viewport[2], viewport[3], GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    // Make the framebuffer blit visible to the following compute texture fetch.
    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    constexpr GLint kHiZTextureUnit = 10;
    glActiveTexture(GL_TEXTURE0 + kHiZTextureUnit);
    glUseProgram(m_gpu_hiz_copy_program);
    glBindTexture(GL_TEXTURE_2D, m_gpu_hiz_depth_texture);
    glUniform1i(m_gpu_loc_hiz_copy_sampler, kHiZTextureUnit);
    glBindImageTexture(0, m_gpu_hiz_texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
    glDispatchCompute(static_cast<GLuint>((width + 7) / 8),
                      static_cast<GLuint>((height + 7) / 8), 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glUseProgram(m_gpu_hiz_reduce_program);
    glUniform1i(m_gpu_loc_hiz_reduce_sampler, kHiZTextureUnit);
    for (int level = 1; level < levels; ++level) {
        const int mip_width = std::max(1, width >> level);
        const int mip_height = std::max(1, height >> level);
        glBindTexture(GL_TEXTURE_2D, m_gpu_hiz_texture);
        glUniform1i(m_gpu_loc_hiz_reduce_source_level, level - 1);
        glBindImageTexture(0, m_gpu_hiz_texture, level, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        glDispatchCompute(static_cast<GLuint>((mip_width + 7) / 8),
                          static_cast<GLuint>((mip_height + 7) / 8), 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }

    glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(original_read_fbo));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(original_draw_fbo));

    m_gpu_hiz_viewport_x = viewport[0];
    m_gpu_hiz_viewport_y = viewport[1];
    m_gpu_hiz_viewport_width = viewport[2];
    m_gpu_hiz_viewport_height = viewport[3];
    m_gpu_hiz_view_projection = view_projection;
    m_gpu_hiz_scene_signature = m_gpu_hiz_pending_signature;
    m_gpu_hiz_valid = true;
}

void Renderer::poll_gpu_cull_stats() {
    // GPU counters are consumed only after a fence signals. A busy slot is left alone;
    // this method never waits for the GPU and therefore cannot stall the frame.
    for (Gpu_Cull_Stats_Readback& readback : m_gpu_cull_stats_readbacks) {
        if (!readback.fence || !readback.buffer) continue;
        const GLenum status = glClientWaitSync(readback.fence, 0, 0);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) continue;

        glBindBuffer(GL_COPY_READ_BUFFER, readback.buffer);
        const auto* values = static_cast<const uint32_t*>(glMapBufferRange(
            GL_COPY_READ_BUFFER, 0, static_cast<GLsizeiptr>(5u * sizeof(uint32_t)), GL_MAP_READ_BIT));
        if (values) {
            m_last_rendered_chunk_count = values[0];
            m_last_rendered_triangle_count = values[1];
            m_last_rendered_section_count = values[2];
            m_last_culled_section_count = values[3];
            m_last_occluded_section_count = values[4];
            glUnmapBuffer(GL_COPY_READ_BUFFER);
        }
        glDeleteSync(readback.fence);
        readback.fence = nullptr;
    }
}

// Frame preparation is GPU-driven after residency updates:
// CPU uploads only chunk candidates and immutable section AABBs/reachability. The compute
// shader performs section frustum culling, optional temporal Hi-Z, transparent-list reversal,
// and writes every DrawArraysIndirect / DrawMeshTasksIndirect command into the GPU buffer.
void Renderer::prepare_gpu_frame(const std::vector<const Renderable_Chunk*>& opaque_order,
                                 bool spectator, const glm::vec3& camera_position,
                                 const glm::mat4& view_projection) {
    poll_gpu_cull_stats();
    poll_gpu_readbacks(false);

    // Chunks with stale mesh data are uploaded in near-to-far order. This work still belongs
    // to the CPU because the world/voxel snapshots are owned by Chunk_Manager; the actual
    // greedy mesh construction remains on the GPU compute shaders.
    const auto collect_pending = [&]() {
        std::vector<const Renderable_Chunk*> pending;
        for (size_t index = 0; index < opaque_order.size(); ++index) {
            const Renderable_Chunk* chunk_ptr = opaque_order[index];
            const Renderable_Chunk& chunk = *chunk_ptr;
            Chunk_GL_Resources& res = *m_frame_res[index];
            if (!chunk.meshes->gpu_upload_pending) {
                release_gpu_chunk(res);
                continue;
            }
            if (res.gpu_handle < 0 || res.gpu_version != chunk.mesh_version || res.gpu_regrow ||
                res.gpu_flora_skipped != chunk.flora_skip_render) {
                pending.push_back(chunk_ptr);
            }
        }
        return pending;
    };

    std::vector<const Renderable_Chunk*> pending = collect_pending();
    // Первое появление чанка важнее фоновой замены уже видимого меша. Внутри обеих
    // групп сохраняется исходный near-to-far порядок из frame_sort.
    std::stable_partition(pending.begin(), pending.end(), [&](const Renderable_Chunk* chunk) {
        const auto it = m_chunk_gl_cache.find(chunk->position);
        return it == m_chunk_gl_cache.end() || it->second.gpu_handle < 0;
    });
    // Rebuilding any resident mesh invalidates the previous depth pyramid: it may contain
    // an occluder that has since been removed or reshaped.
    if (!pending.empty()) m_gpu_hiz_valid = false;
    if (m_gpu_sync_residency) {
        for (int pass = 0; pass < 8 && !pending.empty(); ++pass) {
            for (size_t at = 0; at < pending.size(); at += kGpuBatchChunks) {
                if (m_gpu_readback_count == kGpuReadbackRing) poll_gpu_readbacks(true);
                const size_t finish = std::min(pending.size(), at + static_cast<size_t>(kGpuBatchChunks));
                submit_gpu_batch(std::vector<const Renderable_Chunk*>(
                    pending.begin() + static_cast<std::ptrdiff_t>(at),
                    pending.begin() + static_cast<std::ptrdiff_t>(finish)));
            }
            while (m_gpu_readback_count > 0) poll_gpu_readbacks(true);
            pending = collect_pending();
        }
    } else if (!pending.empty() && m_gpu_readback_count < kGpuReadbackRing) {
        // Streaming uploads share the GPU with the current frame. When the previous
        // timer sample is already above frame budget, shrink this frame's upload batch
        // instead of queueing another large compute dispatch behind an overloaded GPU.
        size_t byte_budget = kGpuFrameBudgetBytes;
        size_t chunk_budget = kGpuFrameBudgetChunks;
        if (m_last_gpu_frame_ms > 28.0f) {
            byte_budget = 512u * 1024u;
            chunk_budget = 8u;
        } else if (m_last_gpu_frame_ms > 20.0f) {
            byte_budget = 1u * 1024u * 1024u;
            chunk_budget = 16u;
        } else if (m_last_gpu_frame_ms > 14.0f) {
            byte_budget = 2u * 1024u * 1024u;
            chunk_budget = 32u;
        }

        std::vector<const Renderable_Chunk*> batch;
        batch.reserve(std::min(chunk_budget, pending.size()));
        size_t bytes = 0;
        for (const Renderable_Chunk* chunk_ptr : pending) {
            if (batch.size() >= chunk_budget) break;
            const size_t chunk_bytes = chunk_ptr->meshes->gpu_words.size() * sizeof(uint32_t);
            if (!batch.empty() && bytes + chunk_bytes > byte_budget) break;
            batch.push_back(chunk_ptr);
            bytes += chunk_bytes;
        }
        if (!batch.empty()) submit_gpu_batch(batch);
    }

    // Optional legacy occlusion queries are polled on the context thread. Their result is
    // only an additional conservative opaque mask; the main camera frustum is never tested
    // on the CPU anymore.
    if (m_occlusion_enabled && !spectator) {
        for (size_t index = 0; index < opaque_order.size(); ++index) {
            const Renderable_Chunk& chunk = *opaque_order[index];
            Chunk_GL_Resources& res = *m_frame_res[index];
            if (res.gpu_handle < 0) continue;
            for (size_t section_index = 0; section_index < chunk.meshes->sections.size(); ++section_index) {
                const Chunk_Section_Range& section = chunk.meshes->sections[section_index];
                const bool opaque = (section.kinds & ((1u << kKindSolid) | (1u << kKindShaped))) != 0;
                if (section.has_geometry && opaque && chunk.section_visible[section_index])
                    poll_occlusion_result(res.occlusion[section_index]);
            }
        }
    }

    std::vector<glm::uvec4> candidates;
    std::vector<glm::vec4> section_bounds;
    candidates.reserve(opaque_order.size());
    section_bounds.reserve(opaque_order.size() * Config::chunk_section_count);

    for (size_t index = 0; index < opaque_order.size(); ++index) {
        const Renderable_Chunk& chunk = *opaque_order[index];
        Chunk_GL_Resources& res = *m_frame_res[index];
        if (res.gpu_handle < 0) continue; // Not resident yet; it will be considered next frame.

        uint32_t reach_mask = 0u;
        uint32_t packed_kinds = 0u;
        uint32_t opaque_occluded_mask = 0u;
        for (size_t section_index = 0; section_index < Config::chunk_section_count; ++section_index) {
            const Chunk_Section_Range& section = chunk.meshes->sections[section_index];
            if (section.has_geometry && chunk.section_visible[section_index])
                reach_mask |= 1u << section_index;
            packed_kinds |= (static_cast<uint32_t>(section.kinds) & 0xFFu) << (section_index * 8u);

            // Section bounds are world-space Y values, matching the uploaded chunk origin.
            // X/Z extents are reconstructed by the GPU from origins[handle].
            section_bounds.emplace_back(section.min_y, section.max_y, 0.0f, 0.0f);

            if (m_occlusion_enabled && !spectator && section.has_geometry &&
                (section.kinds & ((1u << kKindSolid) | (1u << kKindShaped))) != 0) {
                const glm::vec3 min(static_cast<float>(chunk.position.x * Config::chunk_size),
                                    section.min_y,
                                    static_cast<float>(chunk.position.z * Config::chunk_size));
                const glm::vec3 max(min.x + Config::chunk_size, section.max_y, min.z + Config::chunk_size);
                const bool camera_inside = camera_position.x >= min.x && camera_position.x <= max.x &&
                                           camera_position.y >= min.y && camera_position.y <= max.y &&
                                           camera_position.z >= min.z && camera_position.z <= max.z;
                if (camera_inside) res.occlusion[section_index].visible = true;
                if (!res.occlusion[section_index].visible)
                    opaque_occluded_mask |= 1u << section_index;
            }
        }

        if (reach_mask == 0u || packed_kinds == 0u) {
            // This chunk is deliberately not sent to the GPU: it has no reachable geometry.
            // Keep the bounds vector aligned by removing its just-appended section records.
            section_bounds.resize(section_bounds.size() - Config::chunk_section_count);
            continue;
        }
        const uint32_t flags = (chunk.flora_skip_render ? 1u : 0u) | (opaque_occluded_mask << 8u);
        candidates.emplace_back(static_cast<uint32_t>(res.gpu_handle), reach_mask, packed_kinds, flags);
    }

    // No GPU candidates this frame. Clear draw counts so stale indirect commands are never replayed.
    if (candidates.empty()) {
        for (Gpu_Frame_List& list : m_gpu_lists) list = Gpu_Frame_List{};
        m_gpu_hiz_pending_signature = 1469598103934665603ull;
        return;
    }

    // Stable scene signature: temporal Hi-Z is reused only while candidate handles,
    // reachability/kind masks and the actual GPU mesh versions stay identical.
    uint64_t scene_signature = 1469598103934665603ull;
    const auto hash_word = [&scene_signature](uint64_t value) {
        scene_signature ^= value;
        scene_signature *= 1099511628211ull;
    };
    for (size_t i = 0; i < candidates.size(); ++i) {
        const glm::uvec4& candidate = candidates[i];
        hash_word(candidate.x);
        hash_word(candidate.y);
        hash_word(candidate.z);
        hash_word(candidate.w);
        hash_word(static_cast<uint64_t>(opaque_order.empty() ? 0 : i));
    }
    // Include the GPU mesh revision rather than the CPU's pending revision. This keeps
    // a not-yet-uploaded edit from being mistaken for geometry already present in depth.
    for (const Renderable_Chunk* chunk : opaque_order) {
        const auto it = m_chunk_gl_cache.find(chunk->position);
        if (it != m_chunk_gl_cache.end() && it->second.gpu_handle >= 0) {
            hash_word(static_cast<uint32_t>(it->second.gpu_handle));
            hash_word(it->second.gpu_version);
            hash_word(static_cast<uint32_t>(chunk->position.x));
            hash_word(static_cast<uint32_t>(chunk->position.y));
            hash_word(static_cast<uint32_t>(chunk->position.z));
        }
    }
    m_gpu_hiz_pending_signature = scene_signature;

    const size_t candidate_count = candidates.size();
    const size_t order_records = candidate_count * kGpuKindCount;
    const size_t command_records = candidate_count * kGpuKindCount * Config::chunk_section_count;
    const auto ensure_ssbo = [](unsigned int& buffer, size_t& capacity, size_t required,
                                size_t element_size, GLenum usage) {
        if (buffer != 0 && required <= capacity) return;
        const size_t grown = std::max(required, std::max<size_t>(64, capacity * 2));
        if (buffer != 0) glDeleteBuffers(1, &buffer);
        buffer = create_ssbo(grown * element_size, nullptr, usage);
        capacity = grown;
    };

    ensure_ssbo(m_gpu_candidates_ssbo, m_gpu_candidates_capacity, candidate_count,
                sizeof(glm::uvec4), GL_STREAM_DRAW);
    ensure_ssbo(m_gpu_section_bounds_ssbo, m_gpu_bounds_capacity,
                section_bounds.size(), sizeof(glm::vec4), GL_STREAM_DRAW);
    ensure_ssbo(m_gpu_order_ssbo, m_gpu_order_capacity, order_records,
                sizeof(glm::uvec2), GL_STREAM_DRAW);
    ensure_ssbo(m_gpu_cmd_ssbo, m_gpu_cmd_capacity, command_records,
                sizeof(glm::uvec4), GL_STREAM_DRAW);

    const size_t draw_map_records = command_records;
    const size_t draw_meta_required_bytes = 4u * sizeof(uint32_t) + draw_map_records * sizeof(uint32_t);
    if (m_gpu_cmd_meta_ssbo == 0 || draw_meta_required_bytes > m_gpu_cmd_meta_capacity_bytes) {
        const size_t grown_bytes = std::max(draw_meta_required_bytes,
            std::max<size_t>(256u, m_gpu_cmd_meta_capacity_bytes * 2u));
        if (m_gpu_cmd_meta_ssbo != 0) glDeleteBuffers(1, &m_gpu_cmd_meta_ssbo);
        m_gpu_cmd_meta_ssbo = create_ssbo(grown_bytes, nullptr, GL_STREAM_DRAW);
        m_gpu_cmd_meta_capacity_bytes = grown_bytes;
    }
    // Orphan the compact map once per frame so a previous frame's task shaders never
    // serialize this frame's small counter reset with the same storage allocation.
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_cmd_meta_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(m_gpu_cmd_meta_capacity_bytes),
                 nullptr, GL_STREAM_DRAW);
    const uint32_t zero_draw_counts[4] = {};
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zero_draw_counts), zero_draw_counts);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_candidates_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(m_gpu_candidates_capacity * sizeof(glm::uvec4)), nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(candidates.size() * sizeof(glm::uvec4)), candidates.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_section_bounds_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(m_gpu_bounds_capacity * sizeof(glm::vec4)), nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(section_bounds.size() * sizeof(glm::vec4)), section_bounds.data());

    // Set per-kind output offsets. Opaque commands keep the CPU's near-to-far candidate order;
    // the compute shader reverses the two transparent lists on the GPU.
    for (int kind = 0; kind < kGpuKindCount; ++kind) {
        Gpu_Frame_List& list = m_gpu_lists[kind];
        list.order_offset = static_cast<uint32_t>(kind * candidate_count);
        list.cmd_offset = static_cast<uint32_t>(kind * candidate_count * Config::chunk_section_count);
        list.entries = static_cast<uint32_t>(candidate_count);
    }

    const Frustum frustum(view_projection);
    const auto& planes = frustum.planes();
    glUseProgram(m_gpu_cull_program);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_gpu_candidates_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_gpu_section_bounds_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_gpu_counts_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_gpu_ranges_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_gpu_origins_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_gpu_order_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_gpu_cmd_meta_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_gpu_cmd_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, m_gpu_cull_stats_ssbo);
    const uint32_t zero_stats[5] = {};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_cull_stats_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zero_stats), nullptr, GL_STREAM_DRAW);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zero_stats), zero_stats);
    GLint current_viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, current_viewport);
    bool same_view = m_gpu_hiz_valid &&
                     m_gpu_hiz_viewport_x == current_viewport[0] &&
                     m_gpu_hiz_viewport_y == current_viewport[1] &&
                     m_gpu_hiz_viewport_width == current_viewport[2] &&
                     m_gpu_hiz_viewport_height == current_viewport[3] &&
                     scene_signature == m_gpu_hiz_scene_signature && !spectator;
    if (same_view) {
        for (int column = 0; column < 4 && same_view; ++column) {
            for (int row = 0; row < 4; ++row) {
                if (std::abs(view_projection[column][row] - m_gpu_hiz_view_projection[column][row]) > 1.0e-6f) {
                    same_view = false;
                    break;
                }
            }
        }
    }
    const bool use_hiz = same_view && m_gpu_hiz_texture != 0 && m_gpu_hiz_levels > 0;
    glUniform1ui(m_gpu_loc_cull_candidate_count, static_cast<GLuint>(candidate_count));
    glUniform1i(m_gpu_loc_cull_mesh_mode, m_gpu_mesh_shader_ready ? m_gpu_mesh_shader_mode : 0);
    glUniform4fv(m_gpu_loc_cull_planes, static_cast<GLsizei>(planes.size()), &planes[0].x);
    glUniformMatrix4fv(m_gpu_loc_cull_view_projection, 1, GL_FALSE, &view_projection[0][0]);
    glUniform2f(m_gpu_loc_cull_viewport_size, static_cast<float>(current_viewport[2]),
                static_cast<float>(current_viewport[3]));
    glUniform1i(m_gpu_loc_cull_use_hiz, use_hiz ? 1 : 0);
    glUniform1i(m_gpu_loc_cull_compact_opaque, m_gpu_compact_opaque_commands ? 1 : 0);
    if (use_hiz) {
        constexpr GLint kHiZTextureUnit = 10;
        glActiveTexture(GL_TEXTURE0 + kHiZTextureUnit);
        glBindTexture(GL_TEXTURE_2D, m_gpu_hiz_texture);
        glUniform1i(m_gpu_loc_cull_hiz_sampler, kHiZTextureUnit);
        glActiveTexture(GL_TEXTURE0);
    }
    glDispatchCompute(static_cast<GLuint>((candidate_count + 63u) / 64u), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT |
                    GL_BUFFER_UPDATE_BARRIER_BIT |
                    (m_gpu_compact_opaque_commands ? kGpuParameterBufferBarrierBit : 0u));

    // Task/mesh and classic vertex-pulling shaders consume the same GPU-produced order list.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_gpu_quads_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_gpu_counts_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_gpu_order_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_gpu_cmd_meta_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_gpu_ranges_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, m_gpu_rects_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, m_gpu_origins_ssbo);

    // Queue a tiny stats copy into a free ring slot. If all slots are busy, skip this sample
    // rather than waiting; the visible debug counters may lag a frame but rendering never stalls.
    for (size_t offset = 0; offset < m_gpu_cull_stats_readbacks.size(); ++offset) {
        const size_t slot = (m_gpu_cull_stats_write + offset) % m_gpu_cull_stats_readbacks.size();
        Gpu_Cull_Stats_Readback& readback = m_gpu_cull_stats_readbacks[slot];
        if (readback.fence || !readback.buffer) continue;
        glBindBuffer(GL_COPY_READ_BUFFER, m_gpu_cull_stats_ssbo);
        glBindBuffer(GL_COPY_WRITE_BUFFER, readback.buffer);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
                            static_cast<GLsizeiptr>(5u * sizeof(uint32_t)));
        readback.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        m_gpu_cull_stats_write = (slot + 1) % m_gpu_cull_stats_readbacks.size();
        break;
    }
}

void Renderer::draw_gpu_kind(int kind) {
    const Gpu_Frame_List& list = m_gpu_lists[kind];
    if (list.entries == 0) return;
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_gpu_cmd_ssbo);
    glBindVertexArray(m_gpu_section_vao);
    const auto* indirect_offset = reinterpret_cast<const void*>(
        static_cast<uintptr_t>(list.cmd_offset) * sizeof(glm::uvec4));
    const GLsizei draw_count = static_cast<GLsizei>(list.entries * Config::chunk_section_count);

    // Opaque lists are compacted by the culling compute shader. The draw count is read
    // directly from the GPU parameter buffer, so zero-work commands never reach the driver.
    // Transparent flora/liquid retain their fixed reversed ordering for correct blending.
    const bool use_indirect_count = m_gpu_compact_opaque_commands && kind <= kKindShaped;
    if (use_indirect_count) glBindBuffer(kGlParameterBufferArb, m_gpu_cmd_meta_ssbo);

    if (m_gpu_mesh_shader_ready && m_gl_multi_draw_mesh_tasks_indirect) {
        const bool solid = kind == kKindSolid;
        const GLint order_location = solid ? m_gpu_loc_mesh_order_offset_solid
                                           : m_gpu_loc_mesh_order_offset_misc;
        const GLint draw_map_location = solid ? m_gpu_loc_mesh_draw_map_offset_solid
                                              : m_gpu_loc_mesh_draw_map_offset_misc;
        const GLint kind_location = solid ? m_gpu_loc_mesh_kind_solid
                                          : m_gpu_loc_mesh_kind_misc;
        const uint32_t map_offset_words = 4u + static_cast<uint32_t>(kind) *
            list.entries * Config::chunk_section_count;
        glUniform1ui(order_location, m_gpu_lists[kind].order_offset);
        glUniform1ui(draw_map_location, map_offset_words);
        glUniform1i(kind_location, kind);
        // Both EXT and NV take a byte offset (GLintptr), unlike DrawArraysIndirect's host pointer.
        // EXT reads {groupCountX, groupCountY, groupCountZ}; NV reads {taskCount, firstTask}.
        // The fourth word is padding, so both command variants keep a 16-byte stride.
        const GLintptr mesh_indirect_offset = static_cast<GLintptr>(list.cmd_offset) *
                                               static_cast<GLintptr>(sizeof(glm::uvec4));
        if (use_indirect_count && m_gpu_mesh_shader_mode == 1 && m_gl_multi_draw_mesh_tasks_indirect_count) {
            m_gl_multi_draw_mesh_tasks_indirect_count(mesh_indirect_offset,
                static_cast<GLintptr>(kind * sizeof(uint32_t)), draw_count,
                static_cast<GLsizei>(sizeof(glm::uvec4)));
        } else if (use_indirect_count && m_gpu_mesh_shader_mode == 2 &&
                   m_gl_multi_draw_mesh_tasks_indirect_count) {
            m_gl_multi_draw_mesh_tasks_indirect_count(mesh_indirect_offset,
                static_cast<GLintptr>(kind * sizeof(uint32_t)), draw_count,
                static_cast<GLsizei>(sizeof(glm::uvec4)));
        } else {
            m_gl_multi_draw_mesh_tasks_indirect(mesh_indirect_offset, draw_count,
                                                static_cast<GLsizei>(sizeof(glm::uvec4)));
        }
    } else {
        if (kind != kKindSolid && m_gpu_misc_shader_ptr)
            m_gpu_misc_shader_ptr->set_int("u_kind", kind);
        if (use_indirect_count && m_gl_multi_draw_arrays_indirect_count) {
            m_gl_multi_draw_arrays_indirect_count(GL_TRIANGLES,
                static_cast<GLintptr>(list.cmd_offset) * static_cast<GLintptr>(sizeof(glm::uvec4)),
                static_cast<GLintptr>(kind * sizeof(uint32_t)), draw_count,
                static_cast<GLsizei>(sizeof(glm::uvec4)));
        } else {
            glMultiDrawArraysIndirect(GL_TRIANGLES, indirect_offset, draw_count, 0);
        }
    }

    if (use_indirect_count) glBindBuffer(kGlParameterBufferArb, 0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindVertexArray(0);
}

void Renderer::render_chunks(const std::vector<Renderable_Chunk>& chunks,
                            const glm::mat4& view,
                            const glm::mat4& projection,
                            float ambient_intensity,
                            bool spectator,
                            const std::function<void()>& before_liquid,
                            glm::vec3 camera_position,
                            glm::vec3 fog_color,
                            glm::vec3 sun_direction,
                            bool enable_fog) {
    // Non-blocking GPU timer: consume only query results already available; never stall a frame.
    for (Gpu_Frame_Timer_Query& timer : m_gpu_frame_timer_queries) {
        if (!timer.id || !timer.pending) continue;
        GLint available = GL_FALSE;
        glGetQueryObjectiv(timer.id, GL_QUERY_RESULT_AVAILABLE, &available);
        if (available == GL_TRUE) {
            GLuint64 elapsed_ns = 0;
            glGetQueryObjectui64v(timer.id, GL_QUERY_RESULT, &elapsed_ns);
            m_last_gpu_frame_ms = static_cast<float>(static_cast<double>(elapsed_ns) / 1000000.0);
            timer.pending = false;
        }
    }
    bool gpu_timer_started = false;
    Gpu_Frame_Timer_Query& timer_slot = m_gpu_frame_timer_queries[m_gpu_frame_timer_write];
    if (!timer_slot.pending) {
        if (!timer_slot.id) glGenQueries(1, &timer_slot.id);
        if (timer_slot.id) {
            glBeginQuery(GL_TIME_ELAPSED, timer_slot.id);
            gpu_timer_started = true;
        }
    }

    for (Gpu_Frame_List& list : m_gpu_lists) list = Gpu_Frame_List{};

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Occlusion queries are order-dependent within a single frame: a section can only be
    // detected as hidden by geometry that has already written to the depth buffer. Sorting
    // opaque chunks front-to-back (nearest first) means real occluders — the terrain/cave
    // walls actually blocking the view — get drawn before we test what's behind them, so
    // the query for a deep, sealed-off cave section correctly fails once the mountain in
    // front of it has already been rasterized this same frame. Тот же порядок задаёт и очередь
    // загрузки на GPU: ближние чанки получают геометрию первыми.
    // CPU frame preparation is parallel, but deliberately contains no OpenGL calls.
    // First gather stable references on the render thread, then worker threads compute
    // squared distances and sort independent runs. A final merge preserves the old stable
    // ordering for equal distances without forcing every chunk comparison through one core.
    m_frame_sort.clear();
    m_frame_sort.reserve(chunks.size());
    for (size_t index = 0; index < chunks.size(); ++index) {
        if (!chunks[index].meshes) continue;
        m_frame_sort.push_back({0.0f, &chunks[index], index});
    }

    m_frame_workers.parallel_for(m_frame_sort.size(), 96, [&](size_t index) {
        const Renderable_Chunk& chunk = *m_frame_sort[index].chunk;
        const glm::vec3 center(
            (static_cast<float>(chunk.position.x) + 0.5f) * Config::chunk_size,
            (static_cast<float>(chunk.position.y) + 0.5f) * Config::chunk_height,
            (static_cast<float>(chunk.position.z) + 0.5f) * Config::chunk_size);
        const glm::vec3 offset = center - camera_position;
        m_frame_sort[index].distance_sq = glm::dot(offset, offset);
    });

    const auto frame_item_less = [](const Frame_Sort_Item& a, const Frame_Sort_Item& b) {
        if (a.distance_sq != b.distance_sq) return a.distance_sq < b.distance_sq;
        return a.original_order < b.original_order;
    };
    constexpr size_t kParallelSortThreshold = 512;
    constexpr size_t kItemsPerSortRun = 256;
    if (m_frame_sort.size() >= kParallelSortThreshold && m_frame_workers.worker_count() > 0) {
        const size_t run_count = std::min<size_t>(m_frame_workers.worker_count() + 1,
                                                  m_frame_sort.size() / kItemsPerSortRun);
        m_frame_workers.parallel_for(run_count, 1, [&](size_t run) {
            const size_t begin = m_frame_sort.size() * run / run_count;
            const size_t end = m_frame_sort.size() * (run + 1) / run_count;
            std::sort(m_frame_sort.begin() + static_cast<std::ptrdiff_t>(begin),
                      m_frame_sort.begin() + static_cast<std::ptrdiff_t>(end), frame_item_less);
        });
        // Merge runs in increasing powers of two. The tie-break by original_order makes
        // the result equivalent to stable_sort regardless of which worker sorted each run.
        for (size_t width = 1; width < run_count; width *= 2) {
            for (size_t first = 0; first + width < run_count; first += width * 2) {
                const size_t middle = m_frame_sort.size() * (first + width) / run_count;
                const size_t last = m_frame_sort.size() * std::min(run_count, first + width * 2) / run_count;
                std::inplace_merge(m_frame_sort.begin() + static_cast<std::ptrdiff_t>(m_frame_sort.size() * first / run_count),
                                   m_frame_sort.begin() + static_cast<std::ptrdiff_t>(middle),
                                   m_frame_sort.begin() + static_cast<std::ptrdiff_t>(last), frame_item_less);
            }
        }
    } else {
        std::sort(m_frame_sort.begin(), m_frame_sort.end(), frame_item_less);
    }

    std::vector<const Renderable_Chunk*>& opaque_order = m_frame_order;
    opaque_order.clear();
    opaque_order.reserve(m_frame_sort.size());
    // Один поиск в m_chunk_gl_cache на чанк за кадр: указатели на значения unordered_map устойчивы,
    // а остальные проходы кадра (загрузка на GPU, списки секций, occlusion) берут ресурсы по индексу.
    m_frame_res.clear();
    m_frame_res.reserve(m_frame_sort.size());
    for (const Frame_Sort_Item& item : m_frame_sort) {
        opaque_order.push_back(item.chunk);
        Chunk_GL_Resources& res = m_chunk_gl_cache[item.chunk->position];
        res.mesh_version = item.chunk->mesh_version;
        m_frame_res.push_back(&res);
    }
    prepare_gpu_frame(opaque_order, spectator, camera_position, projection * view);

    const float time = static_cast<float>(glfwGetTime());
    const float max_light = static_cast<float>(Config::max_light_level);
    // Fog begins after the nearby gameplay space, then blends into the same colour as
    // the weather/day-night sky. Keep it subtle: it hides the hard render-distance edge
    // without washing out the terrain around the player.
    const float fog_end = std::min(
        std::max(96.0f, static_cast<float>(m_render_distance_chunks * Config::chunk_size)),
        Config::camera_far * 0.90f);
    const glm::vec2 fog_range = enable_fog
        ? glm::vec2(fog_end * 0.70f, fog_end)
        : glm::vec2(100000.0f, 100001.0f);
    sun_direction = glm::length(sun_direction) > 0.001f
        ? glm::normalize(sun_direction) : glm::vec3(0.0f, 1.0f, 0.0f);
    m_blocks_atlas_ptr->bind(0);

    // Непрозрачные кубы. При наличии EXT/NV mesh shading этот draw использует task+mesh
    // stages; иначе остаётся прежний GL 4.3 vertex-pulling pipeline.
    const glm::mat4 view_projection = projection * view;
    if (m_gpu_mesh_shader_ready) {
        glUseProgram(m_gpu_mesh_solid_program);
        glUniformMatrix4fv(m_gpu_loc_mesh_view_projection_solid, 1, GL_FALSE, &view_projection[0][0]);
        glUniform1i(glGetUniformLocation(m_gpu_mesh_solid_program, "textureAtlas"), 0);
        glUniform1f(m_gpu_loc_mesh_time_solid, time);
        glUniform1f(m_gpu_loc_mesh_appear_solid, m_chunk_fade_seconds);
        glUniform1f(m_gpu_loc_mesh_ambient_solid, ambient_intensity);
        glUniform1f(m_gpu_loc_mesh_max_light_solid, max_light);
        glUniform3fv(m_gpu_loc_mesh_camera_solid, 1, &camera_position[0]);
        glUniform3fv(m_gpu_loc_mesh_fog_color_solid, 1, &fog_color[0]);
        glUniform2fv(m_gpu_loc_mesh_fog_range_solid, 1, &fog_range[0]);
        glUniform3fv(m_gpu_loc_mesh_sun_direction_solid, 1, &sun_direction[0]);
    } else {
        m_gpu_terrain_shader_ptr->use();
        m_gpu_terrain_shader_ptr->set_mat4("view_projection", view_projection);
        m_gpu_terrain_shader_ptr->set_int("textureAtlas", 0);
        m_gpu_terrain_shader_ptr->set_float("ambient", ambient_intensity);
        m_gpu_terrain_shader_ptr->set_float("maxLightLevel", max_light);
        m_gpu_terrain_shader_ptr->set_float("time", time);
        m_gpu_terrain_shader_ptr->set_float("appearDuration", m_chunk_fade_seconds);
        m_gpu_terrain_shader_ptr->set_vec3("cameraPosition", camera_position);
        m_gpu_terrain_shader_ptr->set_vec3("fogColor", fog_color);
        m_gpu_terrain_shader_ptr->set_vec2("fogRange", fog_range);
        m_gpu_terrain_shader_ptr->set_vec3("sunDirection", sun_direction);
    }
    draw_gpu_kind(kKindSolid);

    // Остальные виды используют общий shader; u_kind выбирает shaped / flora / liquid.
    const Shader& misc = *m_gpu_misc_shader_ptr;
    const glm::vec4& water_uv = get_block_props(Block_Types::Water).uv_side;
    const glm::vec2 water_origin(water_uv.x, water_uv.y);
    const glm::vec2 water_size(water_uv.z - water_uv.x, water_uv.w - water_uv.y);
    const auto bind_misc_program = [&]() {
        if (m_gpu_mesh_shader_ready) {
            glUseProgram(m_gpu_mesh_misc_program);
            glUniformMatrix4fv(m_gpu_loc_mesh_view_projection_misc, 1, GL_FALSE, &view_projection[0][0]);
            glUniform1i(glGetUniformLocation(m_gpu_mesh_misc_program, "textureAtlas"), 0);
            glUniform1f(m_gpu_loc_mesh_time_misc, time);
            glUniform1f(m_gpu_loc_mesh_appear_misc, m_chunk_fade_seconds);
            glUniform1f(m_gpu_loc_mesh_ambient_misc, ambient_intensity);
            glUniform1f(m_gpu_loc_mesh_max_light_misc, max_light);
            glUniform3fv(m_gpu_loc_mesh_camera_misc, 1, &camera_position[0]);
            glUniform3fv(m_gpu_loc_mesh_fog_color_misc, 1, &fog_color[0]);
            glUniform2fv(m_gpu_loc_mesh_fog_range_misc, 1, &fog_range[0]);
            glUniform3fv(m_gpu_loc_mesh_sun_direction_misc, 1, &sun_direction[0]);
            glUniform2f(m_gpu_loc_mesh_tile_origin_misc, water_origin.x, water_origin.y);
            glUniform2f(m_gpu_loc_mesh_tile_size_misc, water_size.x, water_size.y);
        } else {
            misc.use();
            misc.set_mat4("view_projection", view_projection);
            misc.set_int("textureAtlas", 0);
            misc.set_float("time", time);
            misc.set_float("appearDuration", m_chunk_fade_seconds);
            misc.set_float("ambient", ambient_intensity);
            misc.set_float("maxLightLevel", max_light);
            misc.set_vec3("cameraPosition", camera_position);
            misc.set_vec3("fogColor", fog_color);
            misc.set_vec2("fogRange", fog_range);
            misc.set_vec3("sunDirection", sun_direction);
            misc.set_vec2("tile_origin", water_origin);
            misc.set_vec2("tile_size", water_size);
        }
    };
    bind_misc_program();

    // Блоки формы (ступеньки, плиты): непрозрачные, рисуются вместе с кубами.
    draw_gpu_kind(kKindShaped);

    // Capture the opaque depth and build a max-reduced Hi-Z pyramid on the GPU.
    // It is consumed next frame only if the camera and resident geometry are unchanged.
    build_gpu_hiz(view_projection);
    bind_misc_program();

    // Second pass: now that this frame's opaque draw has written real occluder depth,
    // test every section selected by Chunk_Manager against it. This is what
    // decides whether each section gets drawn NEXT frame — sections whose box is fully
    // behind other terrain (a sealed cave under a hill, the far side of a mountain, etc.)
    // come back "not visible" and are skipped outright, instead of being drawn
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
    // — only its own far underside, well below the real ground. Both passes special-case
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
        for (size_t index = 0; index < opaque_order.size(); ++index) {
            const Renderable_Chunk& chunk = *opaque_order[index];
            auto& gl_res = *m_frame_res[index];
            const glm::vec3 footprint_min(
                static_cast<float>(chunk.position.x * Config::chunk_size), 0.0f,
                static_cast<float>(chunk.position.z * Config::chunk_size));
            const glm::vec3 footprint_max(
                footprint_min.x + static_cast<float>(Config::chunk_size), 0.0f,
                footprint_min.z + static_cast<float>(Config::chunk_size));

            for (size_t section_index = 0; section_index < chunk.meshes->sections.size(); ++section_index) {
                const Chunk_Section_Range& section = chunk.meshes->sections[section_index];
                if (!section.has_geometry || (section.kinds & ((1u << kKindSolid) | (1u << kKindShaped))) == 0 ||
                    !chunk.section_visible[section_index]) continue;

                const glm::vec3 section_min(footprint_min.x, section.min_y, footprint_min.z);
                const glm::vec3 section_max(footprint_max.x, section.max_y, footprint_max.z);

                // Same "camera embedded in this section's box" case as above — skip issuing a query for
                // it and force the state a query would need many frames to converge back to.
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
        bind_misc_program();
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_TRUE);

    // Прозрачные проходы идут по записям списка: чанки от дальних к ближним, а не по секциям —
    // сортировка секций внутри уже отсортированных по глубине чанков дала бы минимальный
    // выигрыш ценой куда более сложного кода.
    // Флора — крестообразные квады, у каждого ОДНА лицевая сторона. При включённом GL_CULL_FACE каждый квад
    // отбрасывался бы с одной из двух сторон, и цветок было бы видно только с двух направлений из четырёх.
    // На время прохода флоры отсечение выключается, и обе стороны квада попадают на экран. Освещение от этого
    // не страдает: фрагментный шейдер смотрит только на NormalY, одинаковый для обеих сторон.
    // Дальность прорисовки флоры (flora_skip_render) учтена при построении списка: трава и цветы — самая мелкая
    // и самая многочисленная геометрия в мире, вдалеке она вырождается в шум из отдельных пикселей.
    glDisable(GL_CULL_FACE);
    draw_gpu_kind(kKindFlora);

    // Наблюдатель может оказаться внутри сплошного блока: у поверхности блока
    // обращённая внутрь сторона является обратной гранью. Оставляем её видимой,
    // иначе при полёте сквозь блок камера видит сквозь его стенки.
    glEnable(GL_CULL_FACE);

    // Непрозрачные объекты (мобы и т.п.) рисуются здесь — ДО полупрозрачной воды, а не после неё.
    // Тогда вода, которая в depth-буфере ближе, смешивается поверх мобов, находящихся под ней.
    // Блендинг на этот проход выключен, как и у остальной непрозрачной геометрии.
    if (before_liquid) {
        glDisable(GL_BLEND);
        before_liquid();
        glEnable(GL_CULL_FACE);
    }

    // Жидкость: волна на верхней грани и мерцание/скролл течения на фрагментах (см. terrain_gpu_misc_*.glsl).
    // Шейдер и ресурсы вернуть после before_liquid: колбэк мог сменить программу и атлас.
    bind_misc_program();
    m_blocks_atlas_ptr->bind(0);

    // Liquid is alpha-blended and sorted back-to-front. It must NOT write depth: a far
    // water surface drawn first would otherwise stamp its depth into the buffer and make
    // the nearer surface fail GL_LESS, which is exactly the "far block covers near block"
    // artifact seen with overlapping semi-transparent blocks. Flora above remains a
    // depth-writing alpha-cutout pass, so solid-looking leaves/grass still occlude correctly.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    draw_gpu_kind(kKindLiquid);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);

    if (gpu_timer_started) {
        glEndQuery(GL_TIME_ELAPSED);
        timer_slot.pending = true;
        m_gpu_frame_timer_write = (m_gpu_frame_timer_write + 1) % m_gpu_frame_timer_queries.size();
    }
}

void Renderer::draw_sky(const glm::vec3& camera_position,
                       const glm::vec3& camera_right,
                       const glm::vec3& camera_up,
                       const glm::mat4& view,
                       const glm::mat4& projection,
                       const glm::vec3& sun_direction,
                       const glm::vec3& moon_direction,
                       float moon_illumination,
                       float sun_darkness,
                       float moon_darkness) {
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

    const auto draw_disc = [&](const glm::vec3& direction, float radius, const glm::vec3& color, float alpha,
                               float illumination, float core) {
        if (alpha <= 0.0f) return;
        const glm::vec3 center = camera_position + direction * distance;
        m_sky_shader_ptr->set_vec3("center", center);
        m_sky_shader_ptr->set_vec3("right", camera_right * radius);
        m_sky_shader_ptr->set_vec3("up", camera_up * radius);
        m_sky_shader_ptr->set_vec3("color", color);
        m_sky_shader_ptr->set_float("alpha", alpha);
        m_sky_shader_ptr->set_float("illumination", illumination);
        m_sky_shader_ptr->set_float("core", core);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    };

    // Мягкое исчезновение под горизонтом (а не резкое пропадание ровно на y=0) — на глаз
    // выглядит естественнее, особенно с учётом того, что диск не идеально маленький.
    const auto fade = [](float elevation) {
        return glm::clamp((elevation + 0.06f) / 0.12f, 0.0f, 1.0f);
    };

    // Солнце: при фазе 32 оно чёрное и окружено ярким диском-проекцией (он темнеет вместе с
    // фазой: sun_darkness 0..1). Ядро чёрное на весь диск при sun_darkness = 1.
    const float sun_fade = fade(sun_direction.y);
    constexpr float halo_radius_scale = 1.6f;
    draw_disc(sun_direction, sun_radius * halo_radius_scale, glm::vec3(1.0f, 1.0f, 0.9f),
              sun_fade * sun_darkness, 2.0f, 0.0f);
    draw_disc(sun_direction, sun_radius, glm::vec3(1.0f, 0.95f, 0.75f), sun_fade, 2.0f, sun_darkness);

    // Луна: яркость растёт к полнолунию (белый), при фазе 32 в центре чёрный диск на половине
    // радиуса (core 0.5 * moon_darkness).
    const glm::vec3 moon_color = glm::mix(glm::vec3(0.85f, 0.87f, 0.92f), glm::vec3(1.0f), moon_darkness);
    draw_disc(moon_direction, moon_radius, moon_color, fade(moon_direction.y), moon_illumination, 0.5f * moon_darkness);

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

