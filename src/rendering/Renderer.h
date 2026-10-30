//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_RENDERER_H
#define OPTICRAFT_RENDERER_H

#include <glad/gl.h>
#include <array>
#include <functional>
#include <map>
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

    // Вид геометрии GPU-мешера: номер совпадает с битом маски вида (Chunk_Section_Range::kinds).
    enum Gpu_Kind { kKindSolid = 0, kKindShaped = 1, kKindFlora = 2, kKindLiquid = 3, kGpuKindCount = 4 };
    // Потоков квадов на чанк: вид * 4 + секция. Один поток — непрерывный участок пула.
    static constexpr int kGpuStreams = kGpuKindCount * Config::chunk_section_count;

    struct Chunk_GL_Resources {
        uint64_t mesh_version = 0;
        std::array<Section_Occlusion_State, Config::chunk_section_count> occlusion{};
        // Ручка чанка в пуле GPU-мешера (-1 = чанк не на GPU) и версия меша, по которой посчитаны квады.
        int gpu_handle = -1;
        uint64_t gpu_version = 0;
        // По потокам: первый квад и ёмкость в квадах (0 = потока нет); желаемая ёмкость после readback'а.
        std::array<uint32_t, kGpuStreams> gpu_first{};
        std::array<uint32_t, kGpuStreams> gpu_cap{};
        std::array<uint32_t, kGpuStreams> gpu_wanted{};
        bool gpu_regrow = false;   // счётчики показали, что ёмкости не хватило: чанк мешится заново
        std::array<bool, kGpuStreams> gpu_capacity_observed{}; // не сжимаем первый allocation каждого потока
        bool gpu_flora_skipped = false; // состав GPU-меша зависит от дистанции прорисовки флоры
    };

    std::unordered_map<glm::ivec3, Chunk_GL_Resources, Chunk_Key_Hash> m_chunk_gl_cache;
    // Буферы кадра переиспользуются между кадрами (без аллокаций): чанки по расстоянию, их порядок и
    // ресурсы — параллельные массивы (m_frame_res[i] относится к m_frame_order[i]).
    struct Frame_Sort_Item { float distance_sq; const Renderable_Chunk* chunk; };
    std::vector<Frame_Sort_Item> m_frame_sort;
    std::vector<const Renderable_Chunk*> m_frame_order;
    std::vector<Chunk_GL_Resources*> m_frame_res;

    void delete_chunk_resources(Chunk_GL_Resources& resources);

    // ---- GPU-мешер: вся геометрия чанков строится видеокартой (OpenGL 4.3, assets/shaders/mesh_*.comp) ----
    // Блоки, метаданные и свет чанка уходят в видеопамять упакованными каналами; mesh_greedy.comp строит жадные
    // квады непрозрачных кубов, mesh_misc.comp — блоки формы, флору и жидкость. Квады лежат в одном общем пуле
    // (по потоку на вид и секцию), а рисуются через glMultiDrawArraysIndirect с vertex pulling.
    // Ёмкость потока растёт и сжимается по настоящему числу квадов: счётчики приходят обратно через readback,
    // и чанк, у которого поток переполнился, мешится заново с нужной ёмкостью (никакого отката на CPU).
    bool initialize_gpu_mesher();
    void shutdown_gpu_mesher();
    void grow_gpu_handles(int needed);
    void grow_gpu_pool(uint32_t extra_blocks);
    int acquire_gpu_handle();
    void release_gpu_chunk(Chunk_GL_Resources& resources);
    uint32_t allocate_quads(uint32_t quads);
    void free_quads(uint32_t first, uint32_t quads);
    // Забирает готовые readback'и счётчиков квадов (FIFO). wait_for_oldest — ждать самый старый.
    void poll_gpu_readbacks(bool wait_for_oldest);
    // Загружает батч чанков в GPU и запускает мешеры на видеокарте.
    void submit_gpu_batch(const std::vector<const Renderable_Chunk*>& batch);
    // Дозагрузка чанков кадра (с бюджетом или целиком), списки видимых секций по видам и команды отрисовки.
    void prepare_gpu_frame(const std::vector<const Renderable_Chunk*>& opaque_order,
                           bool spectator, const glm::vec3& camera_position);
    void draw_gpu_kind(int kind);

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

    // Параметры GPU-мешера. Единица пула — блок из kGpuBlockQuads квадов (16 байт каждый).
    static constexpr int kGpuBatchChunks = 512;
    static constexpr uint32_t kGpuBlockQuads = 32;
    static constexpr uint32_t kGpuInitialPoolBlocks = 16384;  // 512 К квадов = 8 МБ, дальше пул растёт
    // Стартовая ёмкость потока по виду (квады); при переполнении увеличивается по точному readback-счётчику.
    static constexpr std::array<uint32_t, kGpuKindCount> kGpuInitialQuads = {1024, 128, 2048, 1024};
    // Бюджет загрузки за кадр по объёму слов GPU-нагрузки. Ближние чанки грузятся первыми; дальние
    // ждут следующего кадра. Хотя бы один чанк в кадре грузится всегда, даже если он больше бюджета.
    static constexpr size_t kGpuFrameBudgetBytes = 16u * 1024u * 1024u;
    static constexpr int kGpuReadbackRing = 8;
    struct Gpu_Readback {
        unsigned int buffer = 0;
        GLsync fence = nullptr;
        std::vector<glm::ivec3> positions;
        std::vector<uint64_t> versions;
        std::vector<int> handles;
    };
    // Свободные участки пула (в блоках): начало -> длина. Соседние участки сливаются при освобождении.
    struct Gpu_Pool_Allocator {
        std::map<uint32_t, uint32_t> free_runs;
        uint32_t total_blocks = 0;
    };
    // Список отрисовки одного вида за кадр: записи (ручка, маска секций) лежат в общем SSBO порядка.
    struct Gpu_Frame_List {
        uint32_t order_offset = 0;
        uint32_t cmd_offset = 0;
        uint32_t entries = 0;
    };
    bool m_gpu_ready = false;
    bool m_gpu_sync_residency = false;   // true — загрузить всё сразу и дождаться счётчиков (изометрический снимок)
    unsigned int m_gpu_greedy_program = 0;
    unsigned int m_gpu_misc_program = 0;
    unsigned int m_gpu_finalize_program = 0;
    Shader* m_gpu_terrain_shader_ptr = nullptr;   // непрозрачные кубы
    Shader* m_gpu_misc_shader_ptr = nullptr;      // блоки формы, флора, жидкость
    int m_gpu_loc_greedy_chunk_count = -1;
    int m_gpu_loc_misc_chunk_count = -1;
    int m_gpu_loc_finalize_entries = -1;
    int m_gpu_loc_finalize_kind = -1;
    int m_gpu_loc_finalize_order_offset = -1;
    int m_gpu_loc_finalize_cmd_offset = -1;
    unsigned int m_gpu_section_vao = 0;     // VAO с одним атрибутом: baseInstance = ручка*4 + секция, делитель 1
    unsigned int m_gpu_section_ids_vbo = 0; // тождественные номера 0..4*capacity-1
    unsigned int m_gpu_lut_ssbo = 0;        // rect_lut[(id*6 + грань)*256 + метаданные]
    unsigned int m_gpu_tables_ssbo = 0;     // огрубление света (256) и флаги блоков
    unsigned int m_gpu_shape_index_ssbo = 0;// боксы блоков формы: индекс (форма, метаданные) -> (число, смещение)
    unsigned int m_gpu_shape_boxes_ssbo = 0;
    unsigned int m_gpu_rects_ssbo = 0;      // UV-прямоугольники спрайтов (vec4)
    unsigned int m_gpu_headers_ssbo = 0;    // батч: по kGpuHeaderStride uvec4 на чанк
    unsigned int m_gpu_payload_ssbo = 0;    // батч: палитры и упакованные биты
    size_t m_gpu_payload_capacity = 0;      // в байтах
    unsigned int m_gpu_quads_ssbo = 0;      // пул квадов (uvec4)
    unsigned int m_gpu_counts_ssbo = 0;     // число квадов на поток: [ручка * 16 + вид * 4 + секция]
    unsigned int m_gpu_ranges_ssbo = 0;     // место потока в пуле: (первый квад, ёмкость), тот же индекс
    unsigned int m_gpu_origins_ssbo = 0;    // мировой угол чанка на ручку (vec4)
    unsigned int m_gpu_order_ssbo = 0;      // списки отрисовки кадра (ручка, маска секций)
    unsigned int m_gpu_cmd_ssbo = 0;        // команды DrawArraysIndirect: по 4 на запись списка
    size_t m_gpu_order_capacity = 0;        // записей
    size_t m_gpu_cmd_capacity = 0;          // команд
    int m_gpu_handle_capacity = 0;
    int m_gpu_handle_high_water = 0;        // сколько ручек выдано максимум (индекс + 1)
    std::vector<int> m_gpu_free_handles;
    Gpu_Pool_Allocator m_gpu_pool;
    std::vector<uint32_t> m_gpu_stream_counts;   // последние известные счётчики (статистика и подгонка ёмкости)
    std::vector<glm::uvec4> m_gpu_headers_cpu;
    std::vector<uint32_t> m_gpu_payload_cpu;
    std::vector<glm::uvec2> m_gpu_order_cpu;
    std::array<Gpu_Readback, kGpuReadbackRing> m_gpu_readbacks{};
    int m_gpu_readback_head = 0;
    int m_gpu_readback_count = 0;
    std::array<Gpu_Frame_List, kGpuKindCount> m_gpu_lists{};

public:
    void set_occlusion_enabled(bool enabled) { m_occlusion_enabled = enabled; }
    // Изометрический снимок: чанки грузятся на GPU все сразу, кадр ждёт счётчиков квадов (без этого часть
    // чанков доехала бы до видеокарты в следующих кадрах, а снимок должен быть полным).
    void set_gpu_sync_residency(bool enabled) { m_gpu_sync_residency = enabled; }
    // Только для проверок (harness): квады потока (вид, секция) из пула как лежат в видеопамяти (записи по 16 байт).
    // false, если чанк сейчас не на GPU. Читает видеопамять, вне кадра.
    bool debug_gpu_section_quads(const glm::ivec3& chunk_position, int section, std::vector<glm::uvec4>& out,
                                 int kind = 0);
    bool is_occlusion_enabled() const { return m_occlusion_enabled; }

    Renderer();
    ~Renderer();

    // Радиус прорисовки флоры в чанках (-1 = без ограничения). Задаётся из меню
    // настроек; на уже построенные меши не влияет, только на то, какие из них
    // попадают в проход флоры.
    void set_flora_render_distance(int chunks) { m_flora_render_distance_chunks = chunks; }
    int get_flora_render_distance() const { return m_flora_render_distance_chunks; }

    // Загружает шейдеры, поднимает GPU-мешер (нужен OpenGL 4.3) и берёт из Atlas_Registry атлас с именем
    // blocks_atlas_name (по умолчанию "blocks_main") для рендера мировых блоков — Atlas_Registry::load_all()
    // обязан быть вызван ДО этого (см. Game::initialize).
    bool initialize(const std::string& blocks_atlas_name = "blocks_main");

    void clear_screen(float r, float g, float b, float a = 1.0f);

    void render_chunks(const std::vector<Renderable_Chunk>& chunks,
                       const glm::mat4& view,
                       const glm::mat4& projection,
                       float ambient_intensity,
                       bool spectator = false,
                       const std::function<void()>& before_liquid = {});

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

    // Шейдер мобов (skinning) — см. assets/shaders/mob_vertex.glsl / mob_fragment.glsl
    // и src/mobs/Mob.cpp. Нужен вызывающему коду (Game), чтобы передать его в Mob::render().
    Shader* get_mob_shader() { return m_mob_shader_ptr; }
    // Рисование в прямоугольник окна (x, y_top — в пикселях, y_top отсчитывается сверху, как в HUD):
    // свой вьюпорт, своя глубина. end_sub_viewport возвращает полный кадр.
    void begin_sub_viewport(int x, int y_top, int width, int height, int window_height);
    void end_sub_viewport(int window_width, int window_height);
    const Texture_Atlas& get_texture_atlas() const { return *m_blocks_atlas_ptr; }
    size_t get_last_rendered_chunk_count() const { return m_last_rendered_chunk_count; }
    size_t get_last_rendered_triangle_count() const { return m_last_rendered_triangle_count; }
    size_t get_last_rendered_section_count() const { return m_last_rendered_section_count; }
    size_t get_last_culled_section_count() const { return m_last_culled_section_count; }
    size_t get_last_occluded_section_count() const { return m_last_occluded_section_count; }
    void invalidate_chunk_cache(const glm::ivec3& chunk_pos);
};

#endif // OPTICRAFT_RENDERER_H
