//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_RENDERER_H
#define OPTICRAFT_RENDERER_H

#include <glad/gl.h>
#include <array>
#include <cstdint>
#include <vector>
#include <memory>
#include <unordered_map>
#include <glm/glm.hpp>
#include "rendering/Shader.h"
#include "world/chunks/Chunk.h"
#include "world/chunks/Chunk_Manager.h"
#include "utils/Config.h"
#include "math/Hash_Utils.h"
#include "rendering/atlas/Texture_Atlas.h"
#include "rendering/atlas/Atlas_Registry.h"

using Vertex = Chunk_Vertex;

class Renderer {
private:
    Shader* m_shader_ptr;
    Shader* m_liquid_shader_ptr = nullptr;
    Shader* m_flora_shader_ptr = nullptr;
    Shader* m_sky_shader_ptr = nullptr;
    Shader* m_occlusion_shader_ptr = nullptr;
    Shader* m_mob_shader_ptr = nullptr;
    Shader* m_item_shader_ptr = nullptr;
    unsigned int m_item_vao = 0;
    unsigned int m_item_vbo = 0;
    size_t m_item_vbo_capacity = 0; // в вершинах
    unsigned int m_sky_quad_vao = 0;
    unsigned int m_sky_quad_vbo = 0;
    unsigned int m_occlusion_cube_vao = 0;
    unsigned int m_occlusion_cube_vbo = 0;
    // Радиус прорисовки флоры в чанках; -1 = рисовать всю загруженную флору.
    // Меняется из меню настроек (см. Game::cycle_flora_distance).
    int m_flora_render_distance_chunks = Config::flora_render_distance_chunks;

    // Per-section GPU occlusion-query state. Sized to Config::chunk_section_count so it
    // lines up 1:1 with Chunk_Meshes::sections / Chunk_Section_Range.
    struct Section_Occlusion_State {
        unsigned int query = 0;      // GL_ANY_SAMPLES_PASSED query object, 0 = not created yet
        bool query_in_flight = false; // a query was issued last frame we still need to read
        bool visible = true;         // last known/assumed visibility; new sections default true
                                     // so nothing is ever hidden before it's actually been tested
    };

    struct Chunk_GL_Resources {
        unsigned int solid_vbo = 0;
        unsigned int flora_vbo = 0;
        unsigned int liquid_vbo = 0;
        unsigned int solid_vao = 0;
        unsigned int flora_vao = 0;
        unsigned int liquid_vao = 0;
        size_t solid_count = 0;
        size_t flora_count = 0;
        size_t liquid_count = 0;
        uint64_t mesh_version = 0;
        bool is_valid = false;
        std::array<Section_Occlusion_State, Config::chunk_section_count> occlusion{};
        int gpu_slot = -1;         // слот пула GPU-мешера, -1 = чанк не на GPU
        uint64_t gpu_version = 0;  // mesh_version, с которой квады лежат в слоте
    };

    std::unordered_map<glm::ivec3, Chunk_GL_Resources, Chunk_Key_Hash> m_chunk_gl_cache;

    void upload_mesh(unsigned int& vao, unsigned int& vbo, size_t& count, const std::vector<Vertex>& vertices);
    void upload_terrain_mesh(unsigned int& vao, unsigned int& vbo, size_t& count, const std::vector<Terrain_Vertex>& vertices);
    void draw_mesh(unsigned int vao, size_t count);
    void draw_mesh_range(unsigned int vao, size_t start, size_t count);
    void delete_chunk_resources(Chunk_GL_Resources& resources);

    // ---- GPU-мешер непрозрачной геометрии (нужен OpenGL 4.3, см. assets/shaders/mesh_*.comp) ----
    bool initialize_gpu_mesher();
    void shutdown_gpu_mesher();
    void grow_gpu_slots(int needed);
    int acquire_gpu_slot();
    void release_gpu_slot(Chunk_GL_Resources& resources);
    // Забирает готовые readback'и счётчиков квадов (FIFO). wait_for_oldest — ждать самый старый.
    void poll_gpu_readbacks(bool wait_for_oldest);
    // Загружает батч чанков в GPU и запускает greedy-мешер на видеокарте.
    void submit_gpu_batch(const std::vector<const Renderable_Chunk*>& batch);
    // Финализация команд, выбор видимых секций и indirect-отрисовка солидной геометрии GPU-чанков.
    void draw_solid_gpu(const std::vector<const Renderable_Chunk*>& chunks,
                        const glm::mat4& view, const glm::mat4& projection, float ambient_intensity);

    // Reads back last frame's query (if the result is ready) and updates state.visible.
    // Never blocks: if the driver hasn't finished the query yet, last frame's answer is
    // kept and we try again next frame.
    void poll_occlusion_result(Section_Occlusion_State& state);

    // Draws a cheap untextured box covering [min, max] with color/depth writes off, purely
    // to occupy a GL_ANY_SAMPLES_PASSED query against the depth buffer as it stands right
    // now. The result (read back next frame via poll_occlusion_result) tells us whether the
    // section is worth drawing at all next time.
    void issue_occlusion_query(Section_Occlusion_State& state, const glm::mat4& view,
                               const glm::mat4& projection, const glm::vec3& min, const glm::vec3& max);

    // Мировые блоки рендерятся ОДНИМ draw call'ом на меш чанка — значит и текстурой
    // может быть только ОДИН атлас, забинденный на весь этот проход. Он берётся из
    // Atlas_Registry по имени (см. initialize) — Renderer его не грузит и не владеет
    // им, просто хранит указатель для bind() при рендере. Другие атласы (UI и т.п.)
    // получаются через тот же Atlas_Registry напрямую, минуя Renderer.
    const Texture_Atlas* m_blocks_atlas_ptr = nullptr;
    size_t m_last_rendered_chunk_count = 0;
    size_t m_last_rendered_triangle_count = 0;
    size_t m_last_rendered_section_count = 0;
    size_t m_last_culled_section_count = 0;
    size_t m_last_occluded_section_count = 0;
    // false — рисовать все секции, попавшие во фрустум, без occlusion-запросов. Нужно для
    // изометрического снимка: результаты запросов относятся к камере игрока, а не к камере снимка.
    // По умолчанию выключено: тест секции проверяется по глубине, которую рисуют сами секции,
    // поэтому набор видимых секций менялся от кадра к кадру при неподвижной камере (мерцание
    // треугольников у земли). Гистерезис это не закрыл полностью. Включать через
    // set_occlusion_enabled(true) можно только после переделки теста (см. issue_occlusion_query).
    bool m_occlusion_enabled = false;

    // ---- GPU-мешер: видеокарта мешит секции из упакованных блоков чанка (greedy, как в CPU) и
    // рисует квады через glMultiDrawArraysIndirect с vertex pulling (terrain_gpu_vertex.glsl).
    // Пул: слот на чанк, в слоте 4 секции по kGpuQuadCap квадов по 8 байт. Если квадов в секции
    // больше — чанк уходит обратно на CPU-мешер (take_cpu_fallback_requests).
    // 2048: на стартовой сцене (622 непустые секции, макс. 2620 квадов) кап 512 отдавал на CPU 53 из 160
    // чанков, 2048 — 4 из 160. Память пула: 4 * kGpuQuadCap * 8 байт на слот (64 КБ при 2048).
    static constexpr int kGpuQuadCap = 2048;
    static constexpr int kGpuBatchChunks = 64;
    static constexpr int kGpuReadbackRing = 8;
    struct Gpu_Readback {
        unsigned int buffer = 0;
        GLsync fence = nullptr;
        std::vector<glm::ivec3> positions;
        std::vector<uint64_t> versions;
        std::vector<int> slots;
    };
    bool m_gpu_enabled = false;
    unsigned int m_gpu_greedy_program = 0;
    unsigned int m_gpu_finalize_program = 0;
    Shader* m_gpu_terrain_shader_ptr = nullptr;
    int m_gpu_loc_chunk_count = -1;
    int m_gpu_loc_greedy_quad_cap = -1;
    int m_gpu_loc_section_total = -1;
    int m_gpu_loc_finalize_quad_cap = -1;
    unsigned int m_gpu_section_vao = 0;     // VAO с одним атрибутом: номер секции, делитель 1
    unsigned int m_gpu_section_ids_vbo = 0; // тождественные номера 0..4*capacity-1
    unsigned int m_gpu_lut_ssbo = 0;      // rect_lut[(id*6 + грань)*256 + метаданные]
    unsigned int m_gpu_tables_ssbo = 0;   // флаги блоков (256) и огрубление света (256)
    unsigned int m_gpu_rects_ssbo = 0;    // UV-прямоугольники спрайтов (vec4)
    unsigned int m_gpu_headers_ssbo = 0;  // батч: по 16 uvec4 на чанк
    unsigned int m_gpu_payload_ssbo = 0;  // батч: палитры и упакованные биты
    size_t m_gpu_payload_capacity = 0;    // в байтах
    unsigned int m_gpu_quads_ssbo = 0;    // пул: квады [слот*4 + секция][kGpuQuadCap]
    unsigned int m_gpu_counts_ssbo = 0;   // пул: число квадов на секцию
    unsigned int m_gpu_origins_ssbo = 0;  // пул: мировой угол чанка на слот (vec4)
    unsigned int m_gpu_vis_ssbo = 0;      // пул: маска видимых секций на слот
    unsigned int m_gpu_cmd_ssbo = 0;      // команды DrawArraysIndirect на секцию
    int m_gpu_slot_capacity = 0;
    int m_gpu_slot_high_water = 0;        // сколько слотов выдано максимум (индекс + 1)
    std::vector<int> m_gpu_free_slots;
    std::vector<uint32_t> m_gpu_vis_cpu;
    std::vector<uint32_t> m_gpu_quad_counts; // последние известные счётчики квадов (статистика)
    std::vector<glm::uvec4> m_gpu_headers_cpu;
    std::vector<uint32_t> m_gpu_payload_cpu;
    std::array<Gpu_Readback, kGpuReadbackRing> m_gpu_readbacks{};
    int m_gpu_readback_head = 0;
    int m_gpu_readback_count = 0;
    std::vector<glm::ivec3> m_gpu_fallback_requests;

public:
    void set_occlusion_enabled(bool enabled) { m_occlusion_enabled = enabled; }

    // GPU-мешер доступен (GL 4.3 и шейдеры собрались). Иначе весь мешинг остаётся на CPU.
    bool gpu_solid_meshing() const { return m_gpu_enabled; }
    static int gpu_quad_cap() { return kGpuQuadCap; }
    // Чанки, у которых квады не влезли в пул GPU: их надо перестроить на CPU
    // (Chunk_Manager::request_cpu_remesh). Список забирается и очищается.
    std::vector<glm::ivec3> take_cpu_fallback_requests();
    // Только для проверок (harness): квады секции из пула GPU как лежат в видеопамяти (8-байтные записи).
    // false, если чанк сейчас не на GPU (фолбэк на CPU или ещё не загружен). Читает видеопамять, вне кадра.
    bool debug_gpu_section_quads(const glm::ivec3& chunk_position, int section, std::vector<glm::uvec2>& out);
    bool is_occlusion_enabled() const { return m_occlusion_enabled; }

    Renderer();
    ~Renderer();

    // Радиус прорисовки флоры в чанках (-1 = без ограничения). Задаётся из меню
    // настроек; на уже построенные меши не влияет, только на то, какие из них
    // попадают в проход флоры.
    void set_flora_render_distance(int chunks) { m_flora_render_distance_chunks = chunks; }
    int get_flora_render_distance() const { return m_flora_render_distance_chunks; }

    // Загружает шейдеры и берёт из Atlas_Registry атлас с именем blocks_atlas_name
    // (по умолчанию "blocks_main") для рендера мировых блоков — Atlas_Registry::load_all()
    // обязан быть вызван ДО этого (см. Game::initialize).
    bool initialize(const std::string& vertex_shader_path,
                    const std::string& fragment_shader_path,
                    const std::string& blocks_atlas_name = "blocks_main");

    void clear_screen(float r, float g, float b, float a = 1.0f);

    void render_chunks(const std::vector<Renderable_Chunk>& chunks,
                       const glm::mat4& view,
                       const glm::mat4& projection,
                       float ambient_intensity);

    // Рисует солнце и луну как billboard-диски в небе (процедурный шейдер, без текстуры —
    // см. assets/shaders/sky_object_*.glsl). Вызывать ДО render_chunks — рисуется без записи
    // в depth buffer, поэтому обычная непрозрачная геометрия мира естественно перекрывает
    // солнце/луну там, где рельеф закрывает горизонт, без специальной логики окклюзии.
    void draw_sky(const glm::vec3& camera_position,
                  const glm::vec3& camera_right,
                  const glm::vec3& camera_up,
                  const glm::mat4& view,
                  const glm::mat4& projection,
                  const glm::vec3& sun_direction,
                  const glm::vec3& moon_direction);

    // Выпавшие предметы (см. entities/Dropped_Item.h): вершины уже в мировых координатах,
    // пересобираются каждый кадр (предметы крутятся), поэтому VBO динамический.
    void render_dropped_items(const std::vector<Vertex>& vertices,
                              const glm::mat4& view,
                              const glm::mat4& projection,
                              float ambient_intensity);

    Shader* get_shader() { return m_shader_ptr; }
    // Шейдер мобов (skinning) — см. assets/shaders/mob_vertex.glsl / mob_fragment.glsl
    // и src/mobs/Mob.cpp. Нужен вызывающему коду (Game), чтобы передать его в Mob::render().
    Shader* get_mob_shader() { return m_mob_shader_ptr; }
    const Texture_Atlas& get_texture_atlas() const { return *m_blocks_atlas_ptr; }
    size_t get_last_rendered_chunk_count() const { return m_last_rendered_chunk_count; }
    size_t get_last_rendered_triangle_count() const { return m_last_rendered_triangle_count; }
    size_t get_last_rendered_section_count() const { return m_last_rendered_section_count; }
    size_t get_last_culled_section_count() const { return m_last_culled_section_count; }
    size_t get_last_occluded_section_count() const { return m_last_occluded_section_count; }
    void invalidate_chunk_cache(const glm::ivec3& chunk_pos);
};

#endif // OPTICRAFT_RENDERER_H
