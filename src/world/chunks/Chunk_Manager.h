//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_CHUNK_MANAGER_H
#define OPTICRAFT_CHUNK_MANAGER_H

#include <unordered_set>
#include <memory>
#include <limits>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <queue>
#include <thread>
#include <optional>
#include <vector>
#include <glm/glm.hpp>
#include "world/chunks/Chunk.h"
#include "world/chunks/Chunk_Table.h"
#include "world/blocks/Block_Entity_Store.h"
#include "math/Hash_Utils.h"
#include "world/generation/World_Generator.h"
#include "world/storage/World_File.h"
#include "world/fluids/Fluid_Simulator.h"

class Texture_Atlas;
class Frustum;

struct Renderable_Chunk {
    glm::ivec3 position; // (chunk_x, chunk_y, chunk_z)
    const Chunk_Meshes* meshes = nullptr;
    uint64_t mesh_version;
    std::array<bool, Config::chunk_section_count> section_in_frustum{};
    bool flora_skip_render = false;
};

// Chunk_Manager НЕ потокобезопасен в целом: get_chunk/set_block_world/load_around/
// collect_finished_*/rebuild_dirty_chunks и т.п. рассчитаны на вызов ТОЛЬКО с одного
// (главного/рендер) потока — ровно как их и вызывает Game::update_gameplay. Единственное,
// что безопасно трогать из фоновых worker-потоков — это внутренние очереди задач
// (m_pending_jobs/m_pending_mesh_jobs/m_completed_jobs/m_completed_mesh_jobs), защищённые
// своими мьютексами, плюс read-only указатели m_world_generator/m_texture_atlas (выставляются
// один раз при инициализации и не меняются, пока воркеры работают). Проверено стресс-тестом
// под ThreadSanitizer: изначальная версия теста звала set_block_world из отдельного потока
// "как будто это делает игрок в реальном времени" — TSan немедленно поймал гонку на m_chunks,
// потому что это НЕ то, как должен использоваться класс (в игре все правки блоков идут из
// Game::update_gameplay на главном потоке). После исправления теста (все обращения к
// Chunk_Manager — с одного потока, только сами worker'ы понастоящему параллельны) гонок нет.
class Chunk_Manager {
private:
    struct Chunk_Job {
        glm::ivec3 position;
        uint64_t generation_id;
        // Ниже = обрабатывается раньше. Считается один раз при постановке в очередь
        // (enqueue_chunk_load), от позиции игрока НА ТОТ МОМЕНТ — не пересчитывается заново,
        // пока задача уже сидит в очереди (см. комментарий у m_pending_jobs про то, почему
        // это осознанный компромисс, а не недосмотр).
        int priority;
    };

    // priority_queue по умолчанию — max-heap; чтобы наверху оказывалась задача с МЕНЬШИМ
    // priority (= более срочная), сравнение инвертировано.
    struct Chunk_Job_Priority {
        bool operator()(const Chunk_Job& a, const Chunk_Job& b) const {
            return a.priority > b.priority;
        }
    };

    struct Chunk_Result {
        glm::ivec3 position;
        uint64_t generation_id;
        std::unique_ptr<Chunk> chunk;
    };

    // Границы области захвата (см. begin_capture_region), прикладываются к задаче меша в момент
    // постановки. За пределами области соседние чанки НЕ существуют, а не «неизвестны»: воркер
    // трактует их как воздух, поэтому у границы кадра строятся боковые грани и срез мира
    // получается закрытым, а не пустым внутри.
    struct Capture_Clip {
        bool active = false;
        int min_cx = 0, max_cx = 0, min_cz = 0, max_cz = 0;
    };

    // Задача на асинхронную пересборку меша УЖЕ загруженного, разделяемого чанка (правка
    // блока игроком, или новый сосед подгрузился и старые грани на границе устарели).
    // В отличие от Chunk_Job (генерация нового чанка), тут не передаётся живой Chunk —
    // только самодостаточные снимки блоков, скопированные на главном потоке ДО постановки
    // в очередь (см. enqueue_mesh_rebuild) — так воркер не трогает вообще ничего разделяемого
    // и не гоняется за живыми указателями, которые могли бы стать invalid (выгрузка чанка
    // из другого потока никогда не происходит, но сам Chunk мог бы быть удалён, если бы
    // задача хранила указатель на него, а не позицию + копию данных).
    struct Mesh_Job {
        glm::ivec3 position;
        int priority = 0;
        Chunk_Block_Grid own_blocks;
        Chunk_Light_Grid own_light;
        // Порядок: -X, +X, -Y, +Y, -Z, +Z. nullopt = сосед не загружен (тот же фолбэк на
        // World_Generator, что и раньше в живом Chunk_Manager::get_block_world).
        Chunk_Block_Faces neighbor_blocks;
        Chunk_Light_Faces neighbor_light;
        uint64_t generation_id;
        Capture_Clip clip;
        // true — непрозрачные квады строит GPU (нагрузка в Chunk_Meshes, см. build_gpu_solid_payload);
        // false — классический CPU-мешер (тогда в solid лежат готовые вершины).
        bool solid_on_gpu = false;
    };

    struct Mesh_Job_Priority {
        bool operator()(const Mesh_Job& a, const Mesh_Job& b) const { return a.priority > b.priority; }
    };

    struct Mesh_Result {
        glm::ivec3 position;
        uint64_t generation_id;
        Chunk_Meshes meshes;
    };

    // Хранилище загруженных чанков: open-addressing (Robin Hood) вместо chaining hash map —
    // см. подробное обоснование в Chunk_Table.h. get_chunk() вызывается на каждое чтение
    // блока/света и на каждый видимый чанк при рендере каждый кадр, так что устройство ЭТОЙ
    // структуры — самое горячее место во всём Chunk_Manager.
    Chunk_Table m_chunks;
    // Тот же open-addressing, что и у m_chunks, но без нагрузки-значения — только координаты
    // "в полёте": запрошенные к генерации, ожидающие пересборки меша, требующие повторной
    // пересборки. См. Chunk_Coord_Set в Chunk_Table.h.
    Chunk_Coord_Set m_requested_chunks;

    // recompute_sky_lighting() normally runs once the whole load wave has drained (see the
    // comment at its call site) — but m_requested_chunks can stay non-empty indefinitely
    // during continuous exploration (load_around keeps adding new edge chunks faster than
    // they finish) or with a large render distance, so "wait for exactly empty" alone could
    // leave the world dark far longer than intended, or in the worst case never trigger at
    // all for the whole session. This tracks which (chunk_x, chunk_z) columns have gained a
    // newly loaded chunk since the last recompute — packed as (x<<32)|z, same scheme as the
    // column_key used inside recompute_sky_lighting — so collect_finished_chunks can also
    // force a recompute periodically as a fallback, scoped to just those columns rather than
    // the whole loaded world (a full-world redo running several times a second is what
    // previously made the game hang).
    std::unordered_set<uint64_t> m_columns_pending_sky_recompute;
    int m_frames_since_sky_recompute = 0;

    // Горизонтальная позиция игрока в чанках (x, z) — вертикальный столбец загружается
    // целиком (см. Config::world_height_chunks), поэтому раньше здесь Y не хранился. Теперь
    // m_player_chunk_y тоже нужен — не для того, ЧТО грузится (весь столбец грузится
    // по-прежнему), а для того, В КАКОМ ПОРЯДКЕ: чанки рядом с текущей высотой игрока
    // приоритетнее далёкого неба/подземелья (см. Chunk_Job::priority).
    glm::ivec2 m_player_chunk{std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
    int m_player_chunk_y = 0;
    int m_load_radius;

    const World_Generator* m_world_generator = nullptr;
    World_File m_world_file;
    std::string m_generation_folder{"Classic"};
    const Texture_Atlas* m_texture_atlas = nullptr;
    uint64_t m_generation_id = 1;
    std::vector<glm::ivec3> m_unloaded_since_last_query;

    std::vector<std::thread> m_workers;
    // Приоритетная очередь вместо FIFO: см. Chunk_Job::priority. Компаратор читает только
    // поле priority, посчитанное заранее при постановке в очередь — сам компаратор
    // не меняется во времени, так что обычный инвариант std::priority_queue не нарушается
    // (в отличие от гипотетического компаратора, читающего живую позицию игрока "на лету").
    std::priority_queue<Chunk_Job, std::vector<Chunk_Job>, Chunk_Job_Priority> m_pending_jobs;
    std::queue<Chunk_Result> m_completed_jobs;
    std::priority_queue<Mesh_Job, std::vector<Mesh_Job>, Mesh_Job_Priority> m_pending_mesh_jobs;
    std::queue<Mesh_Result> m_completed_mesh_jobs;
    // Чанки, для которых задача пересборки меша прямо сейчас в очереди/выполняется воркером —
    // не даём поставить вторую задачу поверх (бессмысленно дублирует работу).
    Chunk_Coord_Set m_pending_mesh_rebuilds;
    // Если чанк опять поменялся, пока его пересборка уже была в полёте — не теряем правку:
    // помечаем "досчитать ещё раз" и переставляем задачу заново, когда текущая завершится.
    Chunk_Coord_Set m_mesh_rebuild_requeue;
    // Чанки, у которых GPU-мешер переполнил секцию: их меш дальше строится только на CPU.
    Chunk_Coord_Set m_cpu_only_positions;
    // Процесс-глобально: включает GPU-путь для всех менеджеров (см. set_gpu_solid_meshing).
    static inline bool s_gpu_solid_meshing = false;
    std::mutex m_job_mutex;
    std::mutex m_completed_mutex;
    std::condition_variable m_job_available;
    bool m_stop_workers = false;

    // Позиции блоков-источников света (Sun Stone и любой будущий блок с light_emission > 0),
    // сейчас размещённых игроком. Уровень не хранится тут отдельно — при пересчёте читается
    // заново через get_block_world(pos), так что если блок под источником вдруг изменился
    // (не должно происходить штатно, но на всякий случай) — пересчёт сам всё поправит.
    std::vector<glm::ivec3> m_light_sources;

    // Содержимое блоков-контейнеров (сундук/верстак/печь), см. Block_Entity.h — плоская
    // карта по мировым координатам, а не часть Chunk, потому что таких блоков на весь мир
    // единицы по сравнению с обычными. Загружается целиком в set_world_storage (см. .cpp),
    // сохраняется целиком в save_all() — см. Block_Entity_Store.
    Block_Entity_Map m_block_entities;

    // Растекание жидкостей (вода и т.п.): тики назначаются при правке блоков рядом с жидкостью
    // и при загрузке чанков с текущей водой, см. Fluid_Simulator.
    Fluid_Simulator m_fluids;
    // Ставит/связывает/удаляет Block_Entity при постановке/поломке блока-контейнера —
    // вызывается из set_block_world, вынесено отдельно только чтобы не раздувать и без
    // того длинную функцию.
    void sync_block_entity(int wx, int wy, int wz, Block_Types old_type, Block_Types new_type);
    std::filesystem::path get_block_entities_path() const;

    // --- Режим захвата области --------------------------------------------------------------
    // Состояние, пока идёт изометрический снимок (см. публичный блок ниже).
    struct Capture_Region {
        int phase = 0; // Capture_Phase, объявлен ниже в public
        int center_cx = 0, center_cz = 0, radius = 0;
        int extra_depth = 1, top_margin = 32;
        int side = 0;           // 2*radius+1 — колонок в области
        int apron_side = 0;     // side+2 — область плюс кольцо в 1 колонку (для соседей при расчёте дна)
        size_t planned_columns = 0;
        std::vector<int> col_min_h, col_max_h;   // по колонкам с кольцом, apron_side^2
        std::vector<int> bottom_cy, top_cy;      // по колонкам области, side^2
        size_t chunks_total = 0;
        bool remesh_pass_done = false;
        size_t dirty_at_meshing_start = 0;
        size_t lighting_columns_total = 0;
    };
    Capture_Region m_capture;
    bool capture_active() const { return m_capture.phase != 0; }
    bool capture_plan_contains(const glm::ivec3& position) const;
    Capture_Clip current_capture_clip() const;
    void capture_finish_planning();

    void unload_chunk(int cx, int cy, int cz);
    void enqueue_chunk_load(int cx, int cy, int cz);
    bool save_chunk_to_disk(const Chunk& chunk) const;
    void enqueue_mesh_rebuild(int cx, int cy, int cz);
    bool is_chunk_wanted(const glm::ivec3& position) const;
    void start_workers();
    void stop_workers();
    void worker_loop(bool mesh_dedicated = false);

    bool set_light_world(int wx, int wy, int wz, uint8_t level);
    // Полный пересчёт освещения от блоков (не инкрементальный — см. подробный комментарий
    // у реализации в .cpp про то, почему это осознанный выбор, а не недоделка).
    // extra_clear_position: позиция, которую нужно гарантированно включить в область очистки,
    // даже если её уже нет в m_light_sources — нужно для корректного снятия света: когда
    // источник только что убрали, он к этому моменту уже вычеркнут из списка, и без этого
    // параметра его старый свет-"призрак" никогда бы не очистился (см. подробности в .cpp).
    void recompute_lighting(std::optional<glm::ivec3> extra_clear_position = std::nullopt);
    // Scoped to the given (chunk_x, chunk_z) columns (packed (x<<32)|z) rather than the whole
    // loaded world - see the comment at the implementation and at m_columns_pending_sky_recompute.
    void recompute_sky_lighting(const std::unordered_set<uint64_t>& changed_chunk_columns,
                                bool propagate_horizontal = true);

public:
    // Загружен ли чанк (нужен менеджеру посевов: вне загруженной зоны ничего не растёт).
    bool is_chunk_loaded(int cx, int cy, int cz) const;

    explicit Chunk_Manager(int load_radius = Config::chunk_load_radius);
    ~Chunk_Manager();

    void set_world_generator(const World_Generator& generator) {
        m_world_generator = &generator;
    }
    // Загружает world_file/generation_folder И связанный с ним Block_Entity_Store
    // (содержимое сундуков/печей/верстаков этого пресета генерации) — раньше это
    // был тривиальный inline-сеттер двух полей, теперь ещё и файловый I/O, поэтому
    // вынесен в .cpp.
    void set_world_storage(const World_File& world_file, const std::string& generation_folder);
    void set_texture_atlas(const Texture_Atlas& atlas) { m_texture_atlas = &atlas; }

    // Дальность прорисовки/загрузки в чанках. Меняется из меню настроек: чанки за
    // новой границей выгружаются на следующем update_player_position, недостающие
    // ставятся в очередь генерации.
    void set_load_radius(int radius);
    int get_load_radius() const { return m_load_radius; }

    void load_around(int player_cx, int player_cz);
    void unload_distant(int center_cx, int center_cz, int max_distance);
    void update_player_position(float player_x, float player_y, float player_z);
    void collect_finished_chunks();
    // Забирает готовые асинхронные пересборки мешей и применяет их к живым чанкам
    // (или тихо отбрасывает результат, если чанк успели выгрузить, пока задача летала).
    // Нужно звать каждый кадр, как и collect_finished_chunks — см. Game.cpp.
    void collect_finished_mesh_jobs();
    void rebuild_dirty_chunks();
    void clear();
    // Сохраняет все изменённые чанки и сбрасывает индекс region-файла на диск.
    // Зовётся из деструктора (то есть на выходе из игры) и доступна отдельно,
    // если понадобится ручное сохранение мира по кнопке.
    void save_all();

    Chunk* get_chunk(int cx, int cy, int cz);
    const Chunk* get_chunk(int cx, int cy, int cz) const;

    Block_Types get_block_world(int wx, int wy, int wz) const;
    // meta — метаданные нового блока (ориентация/активность, см. Block_Meta); 0 по умолчанию.
    bool set_block_world(int wx, int wy, int wz, Block_Types type, uint8_t meta = 0);
    uint8_t get_block_meta_world(int wx, int wy, int wz) const;
    // Меняет только метаданные уже стоящего блока (печь загорелась, сменила текстуру).
    // Возвращает false, если чанк не загружен или значение не изменилось.
    bool set_block_meta_world(int wx, int wy, int wz, uint8_t meta);
    // 0, если чанк на этой позиции не загружен — свет за пределами загруженного мира просто
    // не считается (в отличие от блоков, для которых есть фолбэк на World_Generator).
    uint8_t get_light_world(int wx, int wy, int wz) const;
    uint8_t get_packed_light_world(int wx, int wy, int wz) const;
    glm::ivec3 get_chunk_coords_for_world(int wx, int wy, int wz) const;

    std::vector<Renderable_Chunk> get_renderable_chunks(
        const Frustum* frustum = nullptr,
        const glm::vec3* camera_position = nullptr,
        int flora_render_distance_chunks = -1
    ) const;
    std::vector<glm::ivec3> take_unloaded_chunk_positions();

    // GPU-путь непрозрачной геометрии (см. Renderer::draw_solid_gpu). Включается рендерером, когда он
    // подтвердил GL 4.3 и скомпилировал шейдеры; действует на все последующие пересборки.
    static void set_gpu_solid_meshing(bool enabled) { s_gpu_solid_meshing = enabled; }
    static bool gpu_solid_meshing() { return s_gpu_solid_meshing; }
    // Чанки, которые GPU-мешер не уместил в секции: пересобрать их на CPU. Уже загруженные
    // позиции; незагруженные молча пропускаются.
    void request_cpu_remesh(const std::vector<glm::ivec3>& positions);

    // Block_Entity блока-контейнера в этой позиции, если он там есть (nullptr иначе).
    // Неконстантная версия — для открытого контейнера (кладём/берём предметы), константная —
    // для рендера/сериализации, которым нельзя случайно его создать/испортить.
    Block_Entity* get_block_entity(int wx, int wy, int wz);
    const Block_Entity* get_block_entity(int wx, int wy, int wz) const;
    // Другая половина двойного сундука, если entity в этой позиции связана (has_pair()) —
    // nullptr, если не связана или партнёр почему-то отсутствует (рассинхрон не должен
    // происходить штатно, но лучше молча вести себя как одиночный сундук, чем упасть).
    Block_Entity* get_paired_block_entity(int wx, int wy, int wz);

    // Тик блоков с внутренним состоянием — сейчас это печи: горение топлива, прожарка,
    // выдача результата. Печь в незагруженном чанке не тикает (как в Minecraft).
    void update_block_entities(float delta_time);

    // Один шаг симуляции жидкостей; звать каждый кадр рядом с update_block_entities.
    void update_fluids(float delta_time);

    // --- Захват области (изометрический снимок мира) -----------------------------------------
    // Грузит и строит меши для квадрата (2R+1)x(2R+1) чанков вокруг (center_cx, center_cz) —
    // намного дальше обычной дальности прорисовки. Чтобы это поместилось в память, по вертикали
    // для каждой колонки берётся только нужный диапазон слоёв (от самой низкой точки рельефа
    // вокруг до верха рельефа плюс запас на деревья), а не всё окно +-8 чанков.
    // Обычная загрузка/выгрузка вокруг игрока на это время отключена; end_capture_region()
    // возвращает всё как было.
    enum class Capture_Phase { Inactive = 0, Planning, Generating, Lighting, Meshing, Ready };
    struct Capture_Status {
        Capture_Phase phase = Capture_Phase::Inactive;
        float progress = 0.0f;     // 0..1 по всем фазам
        size_t chunks_total = 0;
        size_t chunks_loaded = 0;
    };
    void begin_capture_region(int center_cx, int center_cz, int radius,
                              int extra_depth_layers, int top_margin_blocks);
    // Один шаг подготовки области; звать каждый кадр, пока фаза не станет Ready.
    void pump_capture_region();
    Capture_Status get_capture_status() const;
    void end_capture_region();

    size_t get_loaded_count() const { return m_chunks.size(); }
};

#endif //OPTICRAFT_CHUNK_MANAGER_H
