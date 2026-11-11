//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_RENDERER_H
#define OPTICRAFT_RENDERER_H

#include <glad/gl.h>
#include <array>
#include <algorithm>
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
#include "utils/concurrency/Render_Worker_Pool.h"

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
    int m_render_distance_chunks = Config::chunk_load_radius;

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

    struct Gpu_Section_Snapshot {
        float min_y = 0.0f;
        float max_y = 0.0f;
        uint8_t kinds = 0;
        bool has_geometry = false;
    };

    struct Chunk_GL_Resources {
        uint64_t mesh_version = 0;
        std::array<Section_Occlusion_State, Config::chunk_section_count> occlusion{};
        // Ручка чанка в пуле GPU-мешера (-1 = чанк не на GPU) и версия меша, по которой посчитаны квады.
        int gpu_handle = -1;
        uint64_t gpu_version = 0;
        // Версия геометрии, по которой лежат квады в пуле (Chunk_Meshes::geometry_version). Световой проход
        // (light_only) совпадает с ней — квады не пересоздаются, обновляется только световой объём.
        uint64_t gpu_geometry_version = 0;
        uint8_t gpu_lod_stride = 1;
        // По потокам: первый квад и ёмкость в квадах (0 = потока нет); желаемая ёмкость после readback'а.
        std::array<uint32_t, kGpuStreams> gpu_first{};
        std::array<uint32_t, kGpuStreams> gpu_cap{};
        std::array<uint32_t, kGpuStreams> gpu_wanted{};
        bool gpu_regrow = false;   // счётчики показали, что ёмкости не хватило: чанк мешится заново
        std::array<bool, kGpuStreams> gpu_capacity_observed{}; // не сжимаем первый allocation каждого потока
        bool gpu_flora_skipped = false; // состав активного GPU-меша зависит от дистанции флоры
        // Metadata belongs to the active GPU generation, not to the newest CPU snapshot.
        // This lets the previous complete mesh remain correctly culled/drawn while a COW
        // replacement is being built asynchronously.
        std::array<Gpu_Section_Snapshot, Config::chunk_section_count> gpu_sections{};
        bool gpu_sections_valid = false;
        // Copy-on-write меш: пока GPU строит новую версию, старая остаётся активной и продолжает
        // рисоваться. Pending-диапазоны не видны draw-проходу до успешного readback без overflow.
        int gpu_pending_handle = -1;
        uint64_t gpu_pending_version = 0;
        uint64_t gpu_pending_geometry_version = 0;
        uint8_t gpu_pending_lod_stride = 1;
        std::array<uint32_t, kGpuStreams> gpu_pending_first{};
        std::array<uint32_t, kGpuStreams> gpu_pending_cap{};
        std::array<uint32_t, kGpuStreams> gpu_pending_wanted{};
        bool gpu_pending_regrow = false;
        std::array<bool, kGpuStreams> gpu_pending_capacity_observed{};
        bool gpu_pending_flora_skipped = false;
        float gpu_pending_appearance_start_time = 0.0f;
        std::array<Gpu_Section_Snapshot, Config::chunk_section_count> gpu_pending_sections{};
        bool gpu_pending_sections_valid = false;
        // Время первого появления текущей активной GPU-резиденции. Хранится в Origins.w, поэтому
        // не требует отдельного SSBO и не меняет формат quad/payload.
        float appearance_start_time = 0.0f;
    };

    std::unordered_map<glm::ivec3, Chunk_GL_Resources, Chunk_Key_Hash> m_chunk_gl_cache;
    // Буферы кадра переиспользуются между кадрами (без аллокаций): чанки по расстоянию, их порядок и
    // ресурсы — параллельные массивы (m_frame_res[i] относится к m_frame_order[i]).
    struct Frame_Sort_Item { float distance_sq; const Renderable_Chunk* chunk; size_t original_order; };
    // Persistent CPU workers only prepare frame data; all OpenGL commands remain on the context thread.
    Render_Worker_Pool m_frame_workers;
    std::vector<Frame_Sort_Item> m_frame_sort;
    std::vector<const Renderable_Chunk*> m_frame_order;
    std::vector<Chunk_GL_Resources*> m_frame_res;

    void delete_chunk_resources(Chunk_GL_Resources& resources);

    // ---- Постобработка: промежуточный буфер сцены, FXAA и зерно (см. post_fragment.glsl) ----
    float m_chunk_fade_seconds = Config::chunk_fade_seconds_default;
    int m_aa_mode = 0;
    float m_film_grain = 0.0f;
    Shader* m_post_shader_ptr = nullptr;
    unsigned int m_scene_fbo = 0;
    unsigned int m_scene_color = 0;
    unsigned int m_scene_depth = 0;
    int m_scene_width = 0;
    int m_scene_height = 0;
    bool m_scene_active = false;
    int m_saved_framebuffer = 0;
    int m_saved_viewport[4] = {0, 0, 0, 0};
    void ensure_scene_targets(int width, int height);
    void destroy_scene_targets();

    // ---- GPU chunk mesher (OpenGL 4.3 compute + optional EXT/NV mesh shaders) ----
    // Existing compute shaders build compact quad records in one shared pool. On drivers with
    // EXT or NV task/mesh shading those records are expanded into triangles by mesh shader stages;
    // otherwise the classic glMultiDrawArraysIndirect + vertex-pulling path is used.
    // Per-stream capacity follows the real quad counter readback, so an overflow rebuilds only
    // that chunk with a larger allocation (there is no CPU meshing fallback).
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
    // Дозагрузка чанков кадра и GPU-driven culling: CPU передаёт только кандидатов/AABB,
    // compute shader на видеокарте строит visibility masks и indirect draw/task-команды.
    void prepare_gpu_frame(const std::vector<const Renderable_Chunk*>& opaque_order,
                           bool spectator, const glm::vec3& camera_position,
                           const glm::mat4& view_projection);
    void poll_gpu_cull_stats();
    // GPU-таймеры по фазам кадра (glQueryCounter). Опрос неблокирующий: слот читается только когда его
    // последняя метка уже готова. Раз в секунду итог печатается строкой "[GPUSTAT] ..." (см. summarize_trace.py).
    void poll_gpu_phase_timers();
    void gpu_phase_stamp(int stamp);
    void record_gpu_mesher_sample(double elapsed_ms, uint32_t chunk_count);
    void build_gpu_hiz(const glm::mat4& view_projection, bool force_current_depth = false);
    void recull_gpu_frame_with_current_depth(const std::vector<const Renderable_Chunk*>& chunks,
                                             bool spectator, const glm::mat4& view_projection);
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
    struct Gpu_Frame_Timer_Query { GLuint id = 0; bool pending = false; };
    std::array<Gpu_Frame_Timer_Query, 4> m_gpu_frame_timer_queries{};
    size_t m_gpu_frame_timer_write = 0;
    float m_last_gpu_frame_ms = 0.0f;
    // Legacy GL_ANY_SAMPLES_PASSED path only. GPU temporal Hi-Z culling above is independent
    // and uses a conservative previous-frame depth pyramid. The query path stays off by default:
    // its old self-occlusion behavior caused visible flicker and is not needed by GPU Hi-Z.
    // Enables current-frame GPU depth/Hi-Z visibility. Legacy previous-frame section queries
    // are no longer used because their delayed results can hide newly exposed terrain.
    bool m_occlusion_enabled = true;

    // Параметры GPU-мешера. Единица пула — блок из kGpuBlockQuads квадов (16 байт каждый).
    static constexpr int kGpuBatchChunks = 512;
    static constexpr uint32_t kGpuBlockQuads = 32;
    static constexpr uint32_t kGpuInitialPoolBlocks = 16384;  // 512 К квадов = 8 МБ, дальше пул растёт
    // Стартовая ёмкость потока по виду (квады); при переполнении увеличивается по точному readback-счётчику.
    // Liquid sections commonly need one 32x32 surface plus shoreline/side faces. The larger
    // starting range avoids showing a truncated ocean section while its first async readback
    // discovers that the smaller range overflowed.
    static constexpr std::array<uint32_t, kGpuKindCount> kGpuInitialQuads = {1024, 128, 2048, 4096};
    // Hard ceiling for ordinary streaming uploads. Runtime throttling lowers this further
    // when the previous asynchronous GPU timer shows that the chunk pass is already late.
    static constexpr size_t kGpuFrameBudgetBytes = 4u * 1024u * 1024u;
    static constexpr size_t kGpuFrameBudgetChunks = 64u;
    static constexpr int kGpuReadbackRing = 8;
    struct Gpu_Readback {
        unsigned int buffer = 0;
        GLsync fence = nullptr;
        std::vector<glm::ivec3> positions;
        std::vector<uint64_t> versions;
        std::vector<int> handles;
        std::vector<uint8_t> pending_geometry; // 1 = COW-кандидат, 0 = light-only active update
        // Метки GL_TIMESTAMP до и после компьют-диспетчеризации этого батча: по ним считается, сколько
        // миллисекунд GPU стоит один чанк, и от этого зависит размер следующих батчей.
        GLuint stamps[2] = {0, 0};
        uint32_t timed_chunks = 0;   // 0 — батч не измерялся
    };
    // Фазы кадра для GPU-таймеров. Метка пишется после соответствующей фазы.
    enum Gpu_Phase_Stamp {
        kStampStart = 0,   // начало render_chunks
        kStampPrepared,    // после prepare_gpu_frame: загрузка мешей, диспетчеризация мешера, culling
        kStampOpaque,      // после непрозрачных кубов и блоков формы
        kStampHiz,         // после пирамиды Hi-Z
        kStampFlora,       // после флоры
        kStampEnd,         // после мобов и жидкости
        kStampCount
    };
    struct Gpu_Phase_Slot {
        GLuint ids[kStampCount] = {};
        bool pending = false;
    };
    static constexpr int kGpuPhaseSlots = 6;
    // Световой объём ручки: 34 x 34 x 34 байта = 9826 слов (см. mesh_volume.comp).
    static constexpr uint32_t kGpuVolumeWords = 9826u;
    // Сумма за окно в одну секунду реального времени.
    struct Gpu_Stats_Window {
        double begin_time = -1.0;
        uint32_t frames = 0;
        double frame_ms = 0.0, frame_max_ms = 0.0;
        double prepare_ms = 0.0, opaque_ms = 0.0, hiz_ms = 0.0, flora_ms = 0.0, liquid_ms = 0.0;
        uint32_t mesher_batches = 0, mesher_chunks = 0;
        double mesher_ms = 0.0, mesher_max_ms = 0.0;
        // Последнее состояние планировщика мешера (сколько чанков ждёт загрузки и какой был размер батча).
        uint32_t pending_chunks = 0, last_batch_chunks = 0;
        // Батч: световые обновления, локальные секционные пересборки и полные замены геометрии.
        uint32_t light_chunks = 0, partial_chunks = 0, full_chunks = 0;
    };
    // Свободные участки пула (в блоках): начало -> длина. Соседние участки сливаются при освобождении.
    struct Gpu_Pool_Allocator {
        std::map<uint32_t, uint32_t> free_runs;
        uint32_t total_blocks = 0;
    };
    // 32 quads form one culling cluster. The descriptor buffer mirrors the quad pool:
    // descriptor index = firstQuad / 32, so freeing/reusing a quad-pool block reuses its bounds slot.
    struct alignas(16) Gpu_Meshlet_Bounds {
        glm::vec4 minimum{0.0f};
        glm::vec4 maximum{0.0f};
        glm::uvec4 metadata{0u}; // x = actual quad count (0..32)
    };
    struct alignas(16) Gpu_Meshlet_Build_Candidate {
        glm::uvec4 data{}; // handle, stream index, first quad, quad offset in stream
    };
    struct alignas(16) Gpu_Meshlet_Stream {
        glm::uvec4 range{};    // handle, stream index, first quad, bit 0 = LOD (skip conservative Hi-Z)
        glm::uvec4 metadata{}; // section, kind (0 solid / 1 shaped), fixed command offset, meshlet count
    };
    static_assert(sizeof(Gpu_Meshlet_Bounds) == 48);
    static_assert(sizeof(Gpu_Meshlet_Build_Candidate) == 16);
    static_assert(sizeof(Gpu_Meshlet_Stream) == 32);
    struct Gpu_Meshlet_Draw_List {
        uint32_t command_offset = 0; // command record offset, not byte offset
        uint32_t max_commands = 0;
    };
    void build_gpu_meshlet_bounds(const std::vector<Gpu_Meshlet_Build_Candidate>& candidates);
    void rebuild_all_gpu_meshlet_bounds();
    void prepare_gpu_meshlet_frame(const std::vector<const Renderable_Chunk*>& opaque_order,
                                   const glm::mat4& view_projection, bool use_hiz, bool spectator);

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
    unsigned int m_gpu_cull_program = 0;
    unsigned int m_gpu_hiz_copy_program = 0;
    unsigned int m_gpu_hiz_reduce_program = 0;
    // Optional EXT/NV task+mesh pipelines. The GL 4.3 compute/vertex-pulling path remains the fallback.
    unsigned int m_gpu_mesh_solid_program = 0;
    unsigned int m_gpu_mesh_misc_program = 0;
    bool m_gpu_mesh_shader_ready = false;
    // EXT and NV expose the same indirect entry-point signature; mode selects command layout.
    using MultiDrawMeshTasksIndirect = void (GLAD_API_PTR *)(GLintptr, GLsizei, GLsizei);
    using MultiDrawMeshTasksIndirectCount = void (GLAD_API_PTR *)(GLintptr, GLintptr, GLsizei, GLsizei);
    using MultiDrawArraysIndirectCount = void (GLAD_API_PTR *)(GLenum, GLintptr, GLintptr, GLsizei, GLsizei);
    MultiDrawMeshTasksIndirect m_gl_multi_draw_mesh_tasks_indirect = nullptr;
    MultiDrawMeshTasksIndirectCount m_gl_multi_draw_mesh_tasks_indirect_count = nullptr;
    MultiDrawArraysIndirectCount m_gl_multi_draw_arrays_indirect_count = nullptr;
    bool m_gpu_compact_opaque_commands = false;
    int m_gpu_mesh_shader_mode = 0; // 0 classic, 1 EXT, 2 NV
    GLint m_gpu_loc_cull_candidate_count = -1;
    GLint m_gpu_loc_cull_mesh_mode = -1;
    GLint m_gpu_loc_cull_meshlet_enabled = -1;
    GLint m_gpu_loc_cull_planes = -1;
    GLint m_gpu_loc_cull_view_projection = -1;
    GLint m_gpu_loc_cull_viewport_size = -1;
    GLint m_gpu_loc_cull_use_hiz = -1;
    GLint m_gpu_loc_cull_compact_opaque = -1;
    GLint m_gpu_loc_cull_hiz_sampler = -1;
    GLint m_gpu_loc_hiz_copy_sampler = -1;
    GLint m_gpu_loc_hiz_reduce_sampler = -1;
    GLint m_gpu_loc_hiz_reduce_source_level = -1;
    GLint m_gpu_loc_mesh_order_offset_solid = -1;
    GLint m_gpu_loc_mesh_order_offset_misc = -1;
    GLint m_gpu_loc_mesh_draw_map_offset_solid = -1;
    GLint m_gpu_loc_mesh_draw_map_offset_misc = -1;
    GLint m_gpu_loc_mesh_kind_solid = -1;
    GLint m_gpu_loc_mesh_kind_misc = -1;
    GLint m_gpu_loc_mesh_view_projection_solid = -1;
    GLint m_gpu_loc_mesh_view_projection_misc = -1;
    GLint m_gpu_loc_mesh_time_solid = -1;
    GLint m_gpu_loc_mesh_appear_solid = -1;   // appearDuration: длительность появления чанка
    GLint m_gpu_loc_mesh_appear_misc = -1;
    GLint m_gpu_loc_mesh_ambient_solid = -1;
    GLint m_gpu_loc_mesh_ambient_misc = -1;
    GLint m_gpu_loc_mesh_max_light_solid = -1;
    GLint m_gpu_loc_mesh_max_light_misc = -1;
    GLint m_gpu_loc_mesh_time_misc = -1;
    GLint m_gpu_loc_mesh_camera_solid = -1;
    GLint m_gpu_loc_mesh_camera_misc = -1;
    GLint m_gpu_loc_mesh_fog_color_solid = -1;
    GLint m_gpu_loc_mesh_fog_color_misc = -1;
    GLint m_gpu_loc_mesh_fog_range_solid = -1;
    GLint m_gpu_loc_mesh_fog_range_misc = -1;
    GLint m_gpu_loc_mesh_sun_direction_solid = -1;
    GLint m_gpu_loc_mesh_sun_direction_misc = -1;
    GLint m_gpu_loc_mesh_tile_origin_misc = -1;
    GLint m_gpu_loc_mesh_tile_size_misc = -1;
    Shader* m_gpu_terrain_shader_ptr = nullptr;   // непрозрачные кубы
    Shader* m_gpu_misc_shader_ptr = nullptr;      // блоки формы, флора, жидкость
    int m_gpu_loc_greedy_chunk_count = -1;
    int m_gpu_loc_greedy_bake_all = -1;
    int m_gpu_loc_misc_chunk_count = -1;
    unsigned int m_gpu_volume_program = 0;     // mesh_volume.comp: световые объёмы ручек
    int m_gpu_loc_volume_chunk_count = -1;
    unsigned int m_gpu_volume_ssbo = 0;        // объёмы всех ручек подряд: kGpuVolumeWords слов на ручку
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
    unsigned int m_gpu_candidates_ssbo = 0; // (GPU handle, reach mask, packed per-section kinds, flags)
    unsigned int m_gpu_section_bounds_ssbo = 0; // minY/maxY для каждой секции каждого кандидата
    unsigned int m_gpu_order_ssbo = 0;      // GPU-generated per-kind lists: (handle, visible section mask)
    unsigned int m_gpu_cmd_ssbo = 0;        // GPU-generated DrawArrays/MeshTasks indirect commands
    unsigned int m_gpu_cmd_meta_ssbo = 0;   // per-kind indirect counts + compact-to-source draw map
    unsigned int m_gpu_cull_stats_ssbo = 0; // GPU counters; async-copied to readback ring

    // Optional per-meshlet path layered over the existing compact 16-byte quad pool.
    // The normal renderer remains untouched when this switch is off.
    bool m_meshlet_renderer_enabled = false;
    bool m_meshlet_renderer_ready = false;
    bool m_meshlet_bounds_dirty = true;
    bool m_meshlet_compact_commands = false;
    unsigned int m_gpu_meshlet_build_program = 0;
    unsigned int m_gpu_meshlet_cull_program = 0;
    unsigned int m_gpu_meshlet_bounds_ssbo = 0; // one 48-byte descriptor per 32-quad pool block
    unsigned int m_gpu_meshlet_build_ssbo = 0;
    unsigned int m_gpu_meshlet_streams_ssbo = 0;
    unsigned int m_gpu_meshlet_commands_ssbo = 0;
    unsigned int m_gpu_meshlet_counts_ssbo = 0; // indirect draw counts indexed by GPU kind (solid=0, shaped=1, liquid=3)
    size_t m_gpu_meshlet_bounds_capacity_bytes = 0;
    size_t m_gpu_meshlet_build_capacity = 0;
    size_t m_gpu_meshlet_streams_capacity = 0;
    size_t m_gpu_meshlet_commands_capacity = 0; // command records
    std::array<Gpu_Meshlet_Draw_List, kGpuKindCount> m_gpu_meshlet_draw_lists{};
    GLint m_gpu_loc_meshlet_build_count = -1;
    GLint m_gpu_loc_meshlet_build_base = -1;
    GLint m_gpu_loc_meshlet_stream_count = -1;
    GLint m_gpu_loc_meshlet_stream_base = -1;
    GLint m_gpu_loc_meshlet_compact = -1;
    GLint m_gpu_loc_meshlet_solid_base = -1;
    GLint m_gpu_loc_meshlet_shaped_base = -1;
    GLint m_gpu_loc_meshlet_liquid_base = -1;
    GLint m_gpu_loc_meshlet_planes = -1;
    GLint m_gpu_loc_meshlet_view_projection = -1;
    GLint m_gpu_loc_meshlet_viewport = -1;
    GLint m_gpu_loc_meshlet_use_hiz = -1;
    GLint m_gpu_loc_meshlet_hiz = -1;

    size_t m_gpu_candidates_capacity = 0;   // кандидатов
    size_t m_gpu_bounds_capacity = 0;       // vec4 bounds
    size_t m_gpu_order_capacity = 0;        // записей
    size_t m_gpu_cmd_capacity = 0;          // команд
    size_t m_gpu_cmd_meta_capacity_bytes = 0;
    struct Gpu_Cull_Stats_Readback { unsigned int buffer = 0; GLsync fence = nullptr; };
    std::array<Gpu_Cull_Stats_Readback, 3> m_gpu_cull_stats_readbacks{};
    size_t m_gpu_cull_stats_write = 0;
    // Previous-frame hierarchical Z. It is used only while the view, viewport and
    // resident mesh set remain unchanged; otherwise it is invalidated conservatively.
    // Half-resolution depth-only target for current-frame camera-ray visibility. It keeps
    // the visibility prepass cheap on integrated GPUs while the MAX Hi-Z pyramid remains
    // conservative (a coarse/clear sample simply prevents an occlusion decision).
    unsigned int m_gpu_ray_depth_fbo = 0;
    unsigned int m_gpu_ray_depth_texture = 0;
    int m_gpu_ray_depth_width = 0;
    int m_gpu_ray_depth_height = 0;
    unsigned int m_gpu_hiz_depth_texture = 0;
    unsigned int m_gpu_hiz_texture = 0;
    unsigned int m_gpu_hiz_fbo = 0;
    int m_gpu_hiz_width = 0;          // padded texture width (power of two)
    int m_gpu_hiz_height = 0;         // padded texture height (power of two)
    int m_gpu_hiz_viewport_x = 0;
    int m_gpu_hiz_viewport_y = 0;
    int m_gpu_hiz_viewport_width = 0;
    int m_gpu_hiz_viewport_height = 0;
    int m_gpu_hiz_levels = 0;
    bool m_gpu_hiz_valid = false;
    uint64_t m_gpu_hiz_scene_signature = 0;
    uint64_t m_gpu_hiz_pending_signature = 0;
    glm::mat4 m_gpu_hiz_view_projection{1.0f};
    // The max-depth pyramid is expensive at full viewport size. Build it only after
    // the camera and resident opaque mesh set have remained stable for a short window;
    // while the camera is moving, use frustum culling alone and skip the useless rebuild.
    glm::mat4 m_gpu_hiz_candidate_view_projection{1.0f};
    uint64_t m_gpu_hiz_candidate_signature = 0;
    int m_gpu_hiz_candidate_viewport[4] = {0, 0, 0, 0};
    int m_gpu_hiz_stable_frames = 0;
    bool m_gpu_hiz_candidate_valid = false;
    uint32_t m_gpu_frame_candidate_count = 0;
    int m_gpu_handle_capacity = 0;
    int m_gpu_handle_high_water = 0;        // сколько ручек выдано максимум (индекс + 1)
    std::vector<int> m_gpu_free_handles;
    Gpu_Pool_Allocator m_gpu_pool;
    std::vector<uint32_t> m_gpu_stream_counts;   // последние известные счётчики (статистика и подгонка ёмкости)
    std::vector<glm::uvec4> m_gpu_headers_cpu;
    std::vector<uint32_t> m_gpu_payload_cpu;
    std::vector<glm::uvec2> m_gpu_order_cpu; // legacy scratch retained for debug compatibility
    std::array<Gpu_Readback, kGpuReadbackRing> m_gpu_readbacks{};
    std::array<Gpu_Phase_Slot, kGpuPhaseSlots> m_gpu_phase_slots{};
    int m_gpu_phase_current = -1;   // слот, куда пишет текущий кадр (-1: кадр не измеряется)
    int m_gpu_phase_write = 0;
    Gpu_Stats_Window m_gpu_stats{};
    // Скользящее среднее стоимости одного чанка в GPU-мешере (мс), 0 — замеров ещё не было.
    float m_gpu_mesher_ms_per_chunk = 0.0f;
    int m_gpu_readback_head = 0;
    int m_gpu_readback_count = 0;
    std::array<Gpu_Frame_List, kGpuKindCount> m_gpu_lists{};

public:
    void set_occlusion_enabled(bool enabled) { m_occlusion_enabled = enabled; } // toggles current-frame GPU visibility
    // Изометрический снимок: чанки грузятся на GPU все сразу, кадр ждёт счётчиков квадов (без этого часть
    // чанков доехала бы до видеокарты в следующих кадрах, а снимок должен быть полным).
    void set_gpu_sync_residency(bool enabled) { m_gpu_sync_residency = enabled; }
    // Только для проверок (harness): квады потока (вид, секция) из пула как лежат в видеопамяти (записи по 16 байт).
    // false, если чанк сейчас не на GPU. Читает видеопамять, вне кадра.
    bool debug_gpu_section_quads(const glm::ivec3& chunk_position, int section, std::vector<glm::uvec4>& out,
                                 int kind = 0);
    bool is_occlusion_enabled() const { return m_occlusion_enabled; }
    void set_meshlet_renderer_enabled(bool enabled) {
        if (enabled && !m_meshlet_renderer_ready) enabled = false;
        if (m_meshlet_renderer_enabled == enabled) return;
        m_meshlet_renderer_enabled = enabled;
        if (enabled) m_meshlet_bounds_dirty = true; // rebuild descriptors for already resident chunks
    }
    bool is_meshlet_renderer_enabled() const { return m_meshlet_renderer_enabled; }
    bool is_meshlet_renderer_available() const { return m_meshlet_renderer_ready; }

    Renderer();
    ~Renderer();

    // Радиус прорисовки флоры в чанках (-1 = без ограничения). Задаётся из меню
    // настроек; на уже построенные меши не влияет, только на то, какие из них
    // попадают в проход флоры.
    void set_flora_render_distance(int chunks) { m_flora_render_distance_chunks = chunks; }
    void set_render_distance(int chunks) {
        m_render_distance_chunks = std::clamp(chunks, Config::render_distance_min, Config::render_distance_max);
    }
    int get_flora_render_distance() const { return m_flora_render_distance_chunks; }

    // Длительность появления чанка в секундах (блоки проявляются за это время). Из настроек.
    void set_chunk_fade_seconds(float seconds) {
        m_chunk_fade_seconds = std::clamp(seconds, Config::chunk_fade_seconds_min, Config::chunk_fade_seconds_max);
    }
    float get_chunk_fade_seconds() const { return m_chunk_fade_seconds; }

    // Постобработка кадра мира: aa_mode 0 — выкл, 1 — FXAA; grain — доля 0..1 (амплитуда — см. Config).
    // При aa_mode = 0 и grain = 0 мир рисуется прямо в кадр, без лишнего прохода.
    void set_post_processing(int aa_mode, float grain) {
        m_aa_mode = std::clamp(aa_mode, 0, Config::antialiasing_mode_count - 1);
        m_film_grain = std::clamp(grain, 0.0f, 1.0f);
    }
    int get_aa_mode() const { return m_aa_mode; }
    float get_film_grain() const { return m_film_grain; }
    // Между begin_scene и end_scene_and_post мир рисуется в промежуточный буфер. begin_scene возвращает
    // false, если постобработка выключена (тогда end_scene_and_post ничего не делает).
    bool begin_scene(int width, int height);
    void end_scene_and_post();

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
                       const std::function<void()>& before_liquid = {},
                       glm::vec3 camera_position = glm::vec3(0.0f),
                       glm::vec3 fog_color = glm::vec3(0.55f, 0.75f, 0.95f),
                       glm::vec3 sun_direction = glm::vec3(0.35f, 0.85f, 0.25f),
                       bool enable_fog = true);

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
                  const glm::vec3& moon_direction,
                  float moon_illumination,
                  float sun_darkness,
                  float moon_darkness);

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
    float get_last_gpu_frame_ms() const { return m_last_gpu_frame_ms; }
    void invalidate_chunk_cache(const glm::ivec3& chunk_pos);
};

#endif // OPTICRAFT_RENDERER_H
