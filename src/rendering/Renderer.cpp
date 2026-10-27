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

Renderer::Renderer() = default;

Renderer::~Renderer() {
    for (auto& [key, res] : m_chunk_gl_cache) {
        delete_chunk_resources(res);
    }
    shutdown_gpu_mesher();

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

    LOG_INFO("Renderer initialized");
    return true;
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
    m_gpu_finalize_program = compile_compute_program("assets/shaders/mesh_finalize.comp");
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
    if (m_gpu_greedy_program == 0 || m_gpu_misc_program == 0 || m_gpu_finalize_program == 0 ||
        !linked(m_gpu_terrain_shader_ptr) || !linked(m_gpu_misc_shader_ptr)) {
        LOG_ERROR("GPU mesher shader setup failed");
        shutdown_gpu_mesher();
        return false;
    }
    m_gpu_loc_greedy_chunk_count = glGetUniformLocation(m_gpu_greedy_program, "u_chunk_count");
    m_gpu_loc_misc_chunk_count = glGetUniformLocation(m_gpu_misc_program, "u_chunk_count");
    m_gpu_loc_finalize_entries = glGetUniformLocation(m_gpu_finalize_program, "u_entry_count");
    m_gpu_loc_finalize_kind = glGetUniformLocation(m_gpu_finalize_program, "u_kind");
    m_gpu_loc_finalize_order_offset = glGetUniformLocation(m_gpu_finalize_program, "u_order_offset");
    m_gpu_loc_finalize_cmd_offset = glGetUniformLocation(m_gpu_finalize_program, "u_cmd_offset");

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
    LOG_INFO("GPU mesher ready (OpenGL 4.3 compute, unified quad pool)");
    return true;
}

void Renderer::shutdown_gpu_mesher() {
    for (Gpu_Readback& rb : m_gpu_readbacks) {
        if (rb.fence) glDeleteSync(rb.fence);
        if (rb.buffer) glDeleteBuffers(1, &rb.buffer);
        rb = Gpu_Readback{};
    }
    unsigned int buffers[] = {
        m_gpu_lut_ssbo, m_gpu_tables_ssbo, m_gpu_shape_index_ssbo, m_gpu_shape_boxes_ssbo, m_gpu_rects_ssbo,
        m_gpu_headers_ssbo, m_gpu_payload_ssbo, m_gpu_quads_ssbo, m_gpu_counts_ssbo, m_gpu_ranges_ssbo,
        m_gpu_origins_ssbo, m_gpu_order_ssbo, m_gpu_cmd_ssbo, m_gpu_section_ids_vbo
    };
    for (unsigned int id : buffers) {
        if (id != 0) glDeleteBuffers(1, &id);
    }
    if (m_gpu_section_vao != 0) glDeleteVertexArrays(1, &m_gpu_section_vao);
    if (m_gpu_greedy_program != 0) glDeleteProgram(m_gpu_greedy_program);
    if (m_gpu_misc_program != 0) glDeleteProgram(m_gpu_misc_program);
    if (m_gpu_finalize_program != 0) glDeleteProgram(m_gpu_finalize_program);
    delete m_gpu_terrain_shader_ptr;
    delete m_gpu_misc_shader_ptr;

    m_gpu_terrain_shader_ptr = nullptr;
    m_gpu_misc_shader_ptr = nullptr;
    m_gpu_greedy_program = m_gpu_misc_program = m_gpu_finalize_program = 0;
    m_gpu_section_vao = 0;
    m_gpu_section_ids_vbo = 0;
    m_gpu_lut_ssbo = m_gpu_tables_ssbo = m_gpu_shape_index_ssbo = m_gpu_shape_boxes_ssbo = m_gpu_rects_ssbo = 0;
    m_gpu_headers_ssbo = m_gpu_payload_ssbo = 0;
    m_gpu_quads_ssbo = m_gpu_counts_ssbo = m_gpu_ranges_ssbo = m_gpu_origins_ssbo = 0;
    m_gpu_order_ssbo = m_gpu_cmd_ssbo = 0;
    m_gpu_payload_capacity = 0;
    m_gpu_order_capacity = m_gpu_cmd_capacity = 0;
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
    resources.gpu_first.fill(0);
    resources.gpu_cap.fill(0);
    resources.gpu_wanted.fill(0);
    resources.gpu_regrow = false;
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
                    } else if (cap >= 4 * std::max(target, 64u) && cap - target >= 512) {
                        res.gpu_wanted[j] = target;
                        regrow = true;
                    }
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

    for (size_t i = 0; i < n; ++i) {
        const Renderable_Chunk& chunk = *batch[i];
        const Chunk_Meshes& meshes = *chunk.meshes;
        Chunk_GL_Resources& res = m_chunk_gl_cache[chunk.position];
        if (res.gpu_handle < 0) {
            res.gpu_handle = acquire_gpu_handle();
            res.gpu_first.fill(0);
            res.gpu_cap.fill(0);
        }
        res.gpu_version = chunk.mesh_version;
        const size_t handle = static_cast<size_t>(res.gpu_handle);

        // Ёмкости потоков: только там, где секция может дать геометрию этого вида. Размер, подобранный по
        // прошлому readback'у (gpu_wanted) или оставшийся с прошлой версии меша, сохраняется; новому потоку
        // даётся стартовая ёмкость вида.
        std::array<glm::uvec2, kGpuStreams> ranges{};
        for (int k = 0; k < kGpuKindCount; ++k) {
            for (int s = 0; s < Config::chunk_section_count; ++s) {
                const int j = k * Config::chunk_section_count + s;
                uint32_t target = 0;
                if ((meshes.sections[s].kinds >> k) & 1u) {
                    target = res.gpu_wanted[j] != 0 ? res.gpu_wanted[j]
                           : res.gpu_cap[j] != 0 ? res.gpu_cap[j]
                           : kGpuInitialQuads[k];
                    target = (target + kGpuBlockQuads - 1) / kGpuBlockQuads * kGpuBlockQuads;
                }
                if (target != res.gpu_cap[j]) {
                    if (res.gpu_cap[j] != 0) free_quads(res.gpu_first[j], res.gpu_cap[j]);
                    res.gpu_cap[j] = target;
                    res.gpu_first[j] = target != 0 ? allocate_quads(target) : 0;
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
        header[1] = glm::uvec4(meshes.positive_faces_only ? 1u : 0u, static_cast<uint32_t>(meshes.gpu_kinds), 0u, 0u);
        for (int ch = 0; ch < kGpuMeshChannelCount; ++ch) {
            const std::array<uint32_t, 3>& c = meshes.gpu_channels[ch];
            header[2 + ch] = glm::uvec4(base + c[0], base + c[1], c[2], 0u);
        }

        // Мировой угол чанка, диапазоны потоков и обнуление счётчиков.
        const glm::vec4 origin(static_cast<float>(chunk.position.x * Config::chunk_size),
                               static_cast<float>(chunk.position.y * Config::chunk_height),
                               static_cast<float>(chunk.position.z * Config::chunk_size), 0.0f);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_origins_ssbo);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(handle * sizeof(glm::vec4)),
                        sizeof(glm::vec4), &origin);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_ranges_ssbo);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(handle * kGpuStreams * sizeof(glm::uvec2)),
                        sizeof(ranges), ranges.data());
        const std::array<uint32_t, kGpuStreams> zero_counts{};
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_counts_ssbo);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, static_cast<GLintptr>(handle * kGpuStreams * sizeof(uint32_t)),
                        sizeof(zero_counts), zero_counts.data());
        std::fill_n(m_gpu_stream_counts.begin() + static_cast<std::ptrdiff_t>(handle * kGpuStreams), kGpuStreams, 0u);

        rb.positions.push_back(chunk.position);
        rb.versions.push_back(chunk.mesh_version);
        rb.handles.push_back(res.gpu_handle);
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
    for (size_t i = 0; i < n; ++i) {
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                            static_cast<GLintptr>(static_cast<size_t>(rb.handles[i]) * kGpuStreams * sizeof(uint32_t)),
                            static_cast<GLintptr>(i * kGpuStreams * sizeof(uint32_t)),
                            kGpuStreams * sizeof(uint32_t));
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

// Подготовка кадра: догружает на GPU чанки без актуальных квадов (с бюджетом по объёму, ближние первыми),
// собирает для каждого вида список видимых секций и одним финализатором на вид пишет команды отрисовки.
void Renderer::prepare_gpu_frame(const std::vector<const Renderable_Chunk*>& opaque_order,
                                 bool spectator, const glm::vec3& camera_position) {
    poll_gpu_readbacks(false);

    // Чанки без геометрии отдают ручку; остальным, у кого нет актуальных квадов или нужна другая ёмкость,
    // нужна загрузка. opaque_order уже отсортирован от ближних к дальним.
    const auto collect_pending = [&]() {
        std::vector<const Renderable_Chunk*> pending;
        for (const Renderable_Chunk* chunk_ptr : opaque_order) {
            const Renderable_Chunk& chunk = *chunk_ptr;
            Chunk_GL_Resources& res = m_chunk_gl_cache[chunk.position];
            if (!chunk.meshes->gpu_upload_pending) {
                release_gpu_chunk(res);
                continue;
            }
            if (res.gpu_handle < 0 || res.gpu_version != chunk.mesh_version || res.gpu_regrow) {
                pending.push_back(chunk_ptr);
            }
        }
        return pending;
    };

    std::vector<const Renderable_Chunk*> pending = collect_pending();
    if (m_gpu_sync_residency) {
        // Снимок должен быть полным: грузим всё, ждём счётчики и, если какому-то потоку не хватило
        // ёмкости, повторяем (одного повтора достаточно: счётчики настоящие).
        for (int pass = 0; pass < 8 && !pending.empty(); ++pass) {
            for (size_t at = 0; at < pending.size(); at += kGpuBatchChunks) {
                if (m_gpu_readback_count == kGpuReadbackRing) poll_gpu_readbacks(true);
                const size_t end = std::min(pending.size(), at + static_cast<size_t>(kGpuBatchChunks));
                submit_gpu_batch(std::vector<const Renderable_Chunk*>(pending.begin() + static_cast<std::ptrdiff_t>(at),
                                                                      pending.begin() + static_cast<std::ptrdiff_t>(end)));
            }
            while (m_gpu_readback_count > 0) poll_gpu_readbacks(true);
            pending = collect_pending();
        }
    } else if (!pending.empty()) {
        if (m_gpu_readback_count == kGpuReadbackRing) poll_gpu_readbacks(true);
        if (m_gpu_readback_count < kGpuReadbackRing) {
            // Бюджет кадра: pending уже от ближних к дальним, поэтому останавливаемся на первом
            // чанке, который не влезает. Так дальние не обгоняют ближние.
            std::vector<const Renderable_Chunk*> batch;
            size_t bytes = 0;
            for (const Renderable_Chunk* chunk_ptr : pending) {
                if (batch.size() == static_cast<size_t>(kGpuBatchChunks)) break;
                const size_t chunk_bytes = chunk_ptr->meshes->gpu_words.size() * sizeof(uint32_t);
                if (!batch.empty() && bytes + chunk_bytes > kGpuFrameBudgetBytes) break;
                batch.push_back(chunk_ptr);
                bytes += chunk_bytes;
            }
            submit_gpu_batch(batch);
        }
    }

    // Списки видимых секций по видам. Непрозрачные виды (кубы и формы) подчиняются occlusion-запросам, если
    // они включены; флора и жидкость — только фрустуму и достижимости. Порядок записей — порядок отрисовки:
    // непрозрачные — от ближних к дальним, прозрачные — от дальних к ближним.
    std::array<std::vector<glm::uvec2>, kGpuKindCount> entries;
    for (const Renderable_Chunk* chunk_ptr : opaque_order) {
        const Renderable_Chunk& chunk = *chunk_ptr;
        const auto it = m_chunk_gl_cache.find(chunk.position);
        if (it == m_chunk_gl_cache.end() || it->second.gpu_handle < 0) continue;
        Chunk_GL_Resources& res = it->second;
        const Chunk_Meshes& meshes = *chunk.meshes;
        const glm::vec3 footprint_min(static_cast<float>(chunk.position.x * Config::chunk_size), 0.0f,
                                      static_cast<float>(chunk.position.z * Config::chunk_size));
        const glm::vec3 footprint_max(footprint_min.x + static_cast<float>(Config::chunk_size), 0.0f,
                                      footprint_min.z + static_cast<float>(Config::chunk_size));

        std::array<uint32_t, kGpuKindCount> masks{};
        bool any_opaque_drawn = false;
        for (size_t s = 0; s < meshes.sections.size(); ++s) {
            const Chunk_Section_Range& section = meshes.sections[s];
            if (!section.has_geometry) continue;
            const bool opaque_kinds = (section.kinds & ((1u << kKindSolid) | (1u << kKindShaped))) != 0;
            bool opaque_visible = chunk.section_visible[s];
            if (opaque_kinds && !chunk.section_visible[s]) ++m_last_culled_section_count;
            if (opaque_kinds && opaque_visible && m_occlusion_enabled && !spectator) {
                Section_Occlusion_State& occ = res.occlusion[s];
                poll_occlusion_result(occ);
                const glm::vec3 section_min(footprint_min.x, section.min_y, footprint_min.z);
                const glm::vec3 section_max(footprint_max.x, section.max_y, footprint_max.z);
                const bool camera_inside =
                    camera_position.x >= section_min.x && camera_position.x <= section_max.x &&
                    camera_position.y >= section_min.y && camera_position.y <= section_max.y &&
                    camera_position.z >= section_min.z && camera_position.z <= section_max.z;
                if (camera_inside) occ.visible = true;
                if (!occ.visible) {
                    opaque_visible = false;
                    ++m_last_occluded_section_count;
                }
            }
            for (int k = 0; k < kGpuKindCount; ++k) {
                if (!((section.kinds >> k) & 1u)) continue;
                const bool visible = (k <= kKindShaped) ? opaque_visible : chunk.section_visible[s];
                if (!visible) continue;
                if (k == kKindFlora && chunk.flora_skip_render) continue;
                masks[k] |= 1u << s;
                // Статистика: число квадов — из последнего readback'а (с задержкой).
                const int j = k * Config::chunk_section_count + static_cast<int>(s);
                const uint32_t quads = std::min(m_gpu_stream_counts[static_cast<size_t>(res.gpu_handle) * kGpuStreams + j],
                                                res.gpu_cap[j]);
                m_last_rendered_triangle_count += static_cast<size_t>(quads) * 2;
            }
            if (opaque_kinds && opaque_visible) {
                ++m_last_rendered_section_count;
                any_opaque_drawn = true;
            }
        }
        if (any_opaque_drawn) ++m_last_rendered_chunk_count;
        for (int k = 0; k < kGpuKindCount; ++k) {
            if (masks[k] != 0) entries[k].emplace_back(static_cast<uint32_t>(res.gpu_handle), masks[k]);
        }
    }
    std::reverse(entries[kKindFlora].begin(), entries[kKindFlora].end());
    std::reverse(entries[kKindLiquid].begin(), entries[kKindLiquid].end());

    m_gpu_order_cpu.clear();
    uint32_t cmd_total = 0;
    for (int k = 0; k < kGpuKindCount; ++k) {
        Gpu_Frame_List& list = m_gpu_lists[k];
        list.order_offset = static_cast<uint32_t>(m_gpu_order_cpu.size());
        list.cmd_offset = cmd_total;
        list.entries = static_cast<uint32_t>(entries[k].size());
        m_gpu_order_cpu.insert(m_gpu_order_cpu.end(), entries[k].begin(), entries[k].end());
        cmd_total += list.entries * Config::chunk_section_count;
    }
    if (cmd_total == 0) return;

    if (m_gpu_order_cpu.size() > m_gpu_order_capacity) {
        if (m_gpu_order_ssbo != 0) glDeleteBuffers(1, &m_gpu_order_ssbo);
        m_gpu_order_capacity = m_gpu_order_cpu.size() * 2;
        m_gpu_order_ssbo = create_ssbo(m_gpu_order_capacity * sizeof(glm::uvec2), nullptr, GL_STREAM_DRAW);
    }
    if (cmd_total > m_gpu_cmd_capacity) {
        if (m_gpu_cmd_ssbo != 0) glDeleteBuffers(1, &m_gpu_cmd_ssbo);
        m_gpu_cmd_capacity = static_cast<size_t>(cmd_total) * 2;
        m_gpu_cmd_ssbo = create_ssbo(m_gpu_cmd_capacity * sizeof(glm::uvec4), nullptr, GL_STREAM_DRAW);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_order_ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(m_gpu_order_cpu.size() * sizeof(glm::uvec2)),
                    m_gpu_order_cpu.data());

    // Финализация: по одной команде DrawArraysIndirect на (запись списка, секция); count = квады * 6, если секция видна.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glUseProgram(m_gpu_finalize_program);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_gpu_counts_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_gpu_order_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_gpu_ranges_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_gpu_cmd_ssbo);
    for (int k = 0; k < kGpuKindCount; ++k) {
        const Gpu_Frame_List& list = m_gpu_lists[k];
        if (list.entries == 0) continue;
        glUniform1i(m_gpu_loc_finalize_entries, static_cast<GLint>(list.entries));
        glUniform1i(m_gpu_loc_finalize_kind, k);
        glUniform1ui(m_gpu_loc_finalize_order_offset, list.order_offset);
        glUniform1ui(m_gpu_loc_finalize_cmd_offset, list.cmd_offset);
        glDispatchCompute((list.entries * Config::chunk_section_count + 63) / 64, 1, 1);
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

    // Вершины разворачиваются из квадов в вершинном шейдере; общие таблицы привязаны на весь кадр.
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_gpu_quads_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, m_gpu_rects_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, m_gpu_origins_ssbo);
}

void Renderer::draw_gpu_kind(int kind) {
    const Gpu_Frame_List& list = m_gpu_lists[kind];
    if (list.entries == 0) return;
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_gpu_cmd_ssbo);
    glBindVertexArray(m_gpu_section_vao);
    glMultiDrawArraysIndirect(GL_TRIANGLES,
                              reinterpret_cast<const void*>(static_cast<uintptr_t>(list.cmd_offset) * sizeof(glm::uvec4)),
                              static_cast<GLsizei>(list.entries * Config::chunk_section_count), 0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindVertexArray(0);
}

void Renderer::render_chunks(const std::vector<Renderable_Chunk>& chunks,
                            const glm::mat4& view,
                            const glm::mat4& projection,
                            float ambient_intensity,
                            bool spectator,
                            const std::function<void()>& before_liquid) {
    m_last_rendered_chunk_count = 0;
    m_last_rendered_triangle_count = 0;
    m_last_rendered_section_count = 0;
    m_last_culled_section_count = 0;
    m_last_occluded_section_count = 0;
    for (Gpu_Frame_List& list : m_gpu_lists) list = Gpu_Frame_List{};

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    const glm::vec3 camera_position = glm::vec3(glm::inverse(view)[3]);

    // Occlusion queries are order-dependent within a single frame: a section can only be
    // detected as hidden by geometry that has already written to the depth buffer. Sorting
    // opaque chunks front-to-back (nearest first) means real occluders — the terrain/cave
    // walls actually blocking the view — get drawn before we test what's behind them, so
    // the query for a deep, sealed-off cave section correctly fails once the mountain in
    // front of it has already been rasterized this same frame. Тот же порядок задаёт и очередь
    // загрузки на GPU: ближние чанки получают геометрию первыми.
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
        m_chunk_gl_cache[chunk_ptr->position].mesh_version = chunk_ptr->mesh_version;
    }
    prepare_gpu_frame(opaque_order, spectator, camera_position);

    const float time = static_cast<float>(glfwGetTime());
    const float max_light = static_cast<float>(Config::max_light_level);
    m_blocks_atlas_ptr->bind(0);

    // Непрозрачные кубы.
    m_gpu_terrain_shader_ptr->use();
    m_gpu_terrain_shader_ptr->set_mat4("model", glm::mat4(1.0f));
    m_gpu_terrain_shader_ptr->set_mat4("view", view);
    m_gpu_terrain_shader_ptr->set_mat4("projection", projection);
    m_gpu_terrain_shader_ptr->set_int("textureAtlas", 0);
    m_gpu_terrain_shader_ptr->set_float("ambient", ambient_intensity);
    m_gpu_terrain_shader_ptr->set_float("maxLightLevel", max_light);
    draw_gpu_kind(kKindSolid);

    // Остальные виды идут одним шейдером; вид выбирает uniform u_kind.
    const Shader& misc = *m_gpu_misc_shader_ptr;
    misc.use();
    misc.set_mat4("model", glm::mat4(1.0f));
    misc.set_mat4("view", view);
    misc.set_mat4("projection", projection);
    misc.set_int("textureAtlas", 0);
    misc.set_float("time", time);
    misc.set_float("ambient", ambient_intensity);
    misc.set_float("maxLightLevel", max_light);
    // Спрайт воды в UV атласа: левый-нижний угол и размер (скролл и мерцание не выходят за его кусок).
    const glm::vec4& water_uv = get_block_props(Block_Types::Water).uv_side;
    misc.set_vec2("tile_origin", glm::vec2(water_uv.x, water_uv.y));
    misc.set_vec2("tile_size", glm::vec2(water_uv.z - water_uv.x, water_uv.w - water_uv.y));

    // Блоки формы (ступеньки, плиты): непрозрачные, рисуются вместе с кубами.
    misc.set_int("u_kind", 1);
    draw_gpu_kind(kKindShaped);

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
        for (const Renderable_Chunk* chunk_ptr : opaque_order) {
            const Renderable_Chunk& chunk = *chunk_ptr;
            auto& gl_res = m_chunk_gl_cache[chunk.position];
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
        misc.use();
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
    misc.set_int("u_kind", 2);
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
    misc.use();
    m_blocks_atlas_ptr->bind(0);
    misc.set_int("u_kind", 3);

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

