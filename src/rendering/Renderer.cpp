//
// Created by noktemor on 11.02.2026.
//
#include <cstddef>
#include "rendering/Renderer.h"
#include "utils/logging/Logger.h"
#include "utils/Config.h"

#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>

Renderer::Renderer()
    : m_shader_ptr(nullptr)
{}

Renderer::~Renderer() {
    for (auto& [key, res] : m_chunk_gl_cache) {
        delete_chunk_resources(res);
    }

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

void Renderer::render_chunks(const std::vector<Renderable_Chunk>& chunks,
                            const glm::mat4& view,
                            const glm::mat4& projection,
                            float ambient_intensity) {
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
                !chunk.section_in_frustum[section_index]) {
                if (section.has_geometry && section.solid_count > 0 &&
                    !chunk.section_in_frustum[section_index]) {
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
            if (m_occlusion_enabled) {
                poll_occlusion_result(occ);
                if (camera_inside_section) occ.visible = true;
            }

            if (!m_occlusion_enabled || occ.visible) {
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
    if (m_occlusion_enabled) {
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
                !chunk.section_in_frustum[section_index]) continue;

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
                !chunk->section_in_frustum[section_index]) continue;
            draw_mesh_range(gl_res.flora_vao, section.flora_start, section.flora_count);
        }
    }

    // Отсечение обратных граней снова включается: для непрозрачной геометрии и
    // для воды оно по-прежнему нужно.
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
                !chunk->section_in_frustum[section_index]) continue;
            draw_mesh_range(gl_res.liquid_vao, section.liquid_start, section.liquid_count);
        }
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
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
