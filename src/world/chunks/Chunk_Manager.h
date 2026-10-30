//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_CHUNK_MANAGER_H
#define OPTICRAFT_CHUNK_MANAGER_H

#include <unordered_set>
#include <unordered_map>
#include <chrono>
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
#include <variant>
#include <vector>
#include <glm/glm.hpp>
#include "world/chunks/Chunk.h"
#include "world/chunks/Chunk_Table.h"
#include "world/chunks/Chunk_Slot_Pool.h"
#include "world/storage/Chunk_IO.h"
#include "utils/concurrency/Mpmc_Queue.h"
#include "utils/concurrency/Priority_Bucket_Queue.h"
#include <deque>
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
    // Секция рисуется: попадает во фрустум и достижима от камеры через прозрачные клетки (см. get_renderable_chunks).
    std::array<bool, Config::chunk_section_count> section_visible{};
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
        // Ниже = обрабатывается раньше. Считается при постановке в очередь.
        int priority;
    };

    // Работа генерации поставляется по вертикальным колонкам: одна задача может содержать
    // до девяти уровней Y. Это уменьшает очередь/синхронизацию и даёт генератору переиспользовать
    // 2D-поля для всех уровней колонки. Все chunks в задаче имеют одинаковые X/Z.
    struct Chunk_Column_Job {
        int chunk_x = 0;
        int chunk_z = 0;
        uint64_t generation_id = 0;
        int priority = 0;
        std::vector<Chunk_Job> chunks;
    };

    // После однократной подготовки X/Z-данных уровни колонки распараллеливаются внутри
    // того же пула воркеров. Подготовка immutable и живёт, пока последние уровни не готовы.
    struct Chunk_Build_Job {
        Chunk_Job job{};
        Chunk_Ptr chunk{};
        std::shared_ptr<World_Generator::Prepared_Column> prepared{};
        size_t prepared_index = 0;
    };
    struct Region_Prep_Job {
        int chunk_x = 0;
        int chunk_z = 0;
        uint64_t generation_id = 0;
        int priority = 0;
        int64_t region_key = 0;
        uint8_t parts_mask = 0;
    };
    using Generation_Job = std::variant<Chunk_Column_Job, Chunk_Build_Job>;

    struct Chunk_Result {
        glm::ivec3 position;
        uint64_t generation_id;
        Chunk_Ptr chunk;
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
        // Рёбра и углы за границей (кольцо для AO). ring_valid = false: соседа не было при постановке,
        // клетку отдаст генератор в воркере, как и грани без снимка.
        std::array<Block_ID, Chunk_Ring::kSize> ring_blocks{};
        std::array<bool, Chunk_Ring::kSize> ring_valid{};
        uint64_t generation_id;
        // Счётчик пометок чанка на момент снимка (см. Chunk::get_dirty_epoch): при применении сравнивается.
        uint64_t dirty_epoch = 0;
        // Объект чанка, для которого снят снимок (см. Chunk::get_instance_id). Результат применяется
        // только к нему: на позиции мог появиться другой чанк, пока задача летала в воркере.
        uint64_t instance_id = 0;
        Capture_Clip clip;
        // Срочная пересборка (правка блока игроком): идёт первой в очереди и возвращается по отдельной очереди результатов.
        bool urgent = false;
        uint64_t seq = 0;
    };

    struct Mesh_Result {
        glm::ivec3 position;
        uint64_t generation_id;
        Chunk_Meshes meshes;
        uint64_t dirty_epoch = 0;
        uint64_t instance_id = 0;
        uint64_t seq = 0;
        bool urgent = false;
    };

    // Хранилище загруженных чанков: open-addressing (Robin Hood) вместо chaining hash map —
    // см. подробное обоснование в Chunk_Table.h. get_chunk() вызывается на каждое чтение
    // блока/света и на каждый видимый чанк при рендере каждый кадр, так что устройство ЭТОЙ
    // структуры — самое горячее место во всём Chunk_Manager.
    // Слоты под чанки. Объявлен ДО m_chunks и очереди результатов: уничтожается последним, поэтому
    // делетеры Chunk_Ptr всегда находят живой пул.
    std::unique_ptr<Chunk_Slot_Pool> m_chunk_pool;
    // Буферный слой диска: пачками пишет и читает регионы, не заставляя главный поток ждать диск.
    std::unique_ptr<Chunk_IO> m_chunk_io;
    Chunk_Table m_chunks;
    // Меняется при загрузке, выгрузке и замене чанков: по нему кэш обхода достижимости знает, что набор изменился.
    uint64_t m_chunk_set_epoch = 0;
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

    // Движение игрока: скорость в блоках/с, сглаженная по кадрам. Нужна приоритету загрузки
    // (чанки впереди идут раньше) и выбору вертикального окна.
    glm::vec3 m_player_velocity{0.0f};
    glm::vec3 m_last_player_pos{0.0f};
    std::chrono::steady_clock::time_point m_last_pos_time{};
    bool m_has_last_pos = false;
    // Полуширина вертикального окна в чанках (оба края включительно): обычно 4 (9 уровней),
    // при быстром вертикальном движении или малой дальности прорисовки — 2 (5 уровней).
    int m_vertical_half = 4;
    // Остались ли в окне незапрошенные чанки (load_around упёрся в лимит очереди). Тогда
    // update_player_position доливает их по мере освобождения очереди.
    bool m_load_backlog = false;

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
    // Задачи генерации: корзины по приоритету, без замков (см. Priority_Bucket_Queue).
    // Regional cache preparation must not be starved behind a continuous mesh queue.
    // It is a small, mutex-protected priority deque and is popped before mesh jobs.
    std::deque<Region_Prep_Job> m_pending_region_prep_jobs;
    Priority_Bucket_Queue<Generation_Job> m_pending_jobs;
    std::atomic<size_t> m_pending_job_count{0};
    std::atomic<int> m_sleepers{0};
    void wake_workers();
    // Поколение вида: растёт при каждой смене чанка игрока. Кольца загруженных чанков пересчитываются
    // под него; меш ставится с приоритетом по кольцу, а кольца 0–1 без меша уходят сразу.
    uint32_t m_view_generation = 0;
    // Чанки, чьи секции реально попали в текущий frustum/visibility traversal.
    // Обновляется get_renderable_chunks на главном потоке и используется как приоритет мешинга.
    mutable Chunk_Coord_Set m_visible_chunks;
    // Плоский список загруженных чанков для обхода кадра: пересобирается только при смене набора
    // (m_chunk_set_epoch), а не каждый кадр обходом хеш-таблицы.
    mutable std::vector<std::pair<glm::ivec3, Chunk*>> m_loaded_cache;
    mutable uint64_t m_loaded_cache_epoch = ~0ull;
    // Скан грязных чанков (rebuild_dirty_chunks) пропускается, пока с прошлого скана, не нашедшего
    // ни одного грязного чанка, никто не стал грязным и набор чанков не менялся.
    bool m_dirty_scan_clean = false;
    uint64_t m_dirty_scan_events = 0;
    uint64_t m_dirty_scan_set_epoch = 0;
    uint8_t view_ring_of(const glm::ivec3& position) const;
    void refresh_view_rings();
    Mpmc_Queue<Chunk_Result> m_completed_jobs;
    // Задачи загрузки, которые есть на диске: их читает m_disk_reader пачками (см. Chunk_IO).
    std::deque<Chunk_Job> m_pending_disk_reads;
    std::thread m_disk_reader;
    // Задачи мешинга: приоритет = кольцо * 2048 + квадрат расстояния (до 2047), то есть до 34816 при
    // кольце 16. Корзины шириной 16 покрывают весь диапазон (2176 штук); при шаге 1 и 1024 корзинах
    // все кольца от 1 и дальше попадали в одну последнюю корзину и шли FIFO, без порядка по кольцам.
    // Сами задачи лежат за unique_ptr: Mesh_Job крупный, а ячейки кольца хранят указатель.
    Priority_Bucket_Queue<std::unique_ptr<Mesh_Job>, 16, 2176, 64> m_pending_mesh_jobs;
    std::atomic<size_t> m_pending_mesh_count{0};
    Mpmc_Queue<Mesh_Result> m_completed_mesh_jobs;
    // Результаты срочных пересборок (правка блока): применяются раньше обычных и без лимита на кадр.
    Mpmc_Queue<Mesh_Result> m_completed_urgent_mesh_jobs;
    // Чанки, которые игрок правил в этом кадре: пересборка ставится один раз за кадр после всех правок.
    Chunk_Coord_Set m_urgent_mesh_set;
    std::vector<glm::ivec3> m_urgent_mesh_list;
    void request_urgent_mesh_rebuild(int cx, int cy, int cz);
    void flush_urgent_mesh_rebuilds();
    // Наименьшее кольцо, где ещё есть нужный, но незагруженный чанк. Меши строятся строго от центра к краю.
    int first_incomplete_ring();
    uint64_t m_ring_cache_epoch = ~0ull;
    uint32_t m_ring_cache_view = ~0u;
    int m_ring_cache_half = -1;
    size_t m_ring_cache_inflight = ~size_t(0);
    int m_ring_cache_value = 0;
    // Осталась ли выгрузка, не уложившаяся в лимит кадра.
    bool m_unload_backlog = false;
    // Чанки, для которых задача пересборки меша прямо сейчас в очереди/выполняется воркером —
    // не даём поставить вторую задачу поверх (бессмысленно дублирует работу).
    Chunk_Coord_Set m_pending_mesh_rebuilds;
    // Если чанк опять поменялся, пока его пересборка уже была в полёте — не теряем правку:
    // помечаем "досчитать ещё раз" и переставляем задачу заново, когда текущая завершится.
    Chunk_Coord_Set m_mesh_rebuild_requeue;
    std::mutex m_job_mutex;
    std::condition_variable m_job_available;
    bool m_stop_workers = false;

    // Cold regional cave/hydrology calculations are scheduled as distinct jobs in the same
    // worker pool. Columns waiting for a missing region sit here; only warm columns enter
    // the ordinary generation queue. This prevents all workers from blocking on one future.
    std::unordered_map<int64_t, std::vector<Chunk_Column_Job>> m_region_waiting_columns;
    std::unordered_map<int64_t, uint64_t> m_regions_preparing;
    // Bits scheduled for this region/generation: caves=1, rivers=2. They can build in parallel.
    std::unordered_map<int64_t, uint8_t> m_region_prep_queued_mask;

    // Позиции блоков-источников света (Sun Stone и любой будущий блок с light_emission > 0),
    // сейчас размещённых игроком. Уровень не хранится тут отдельно — при пересчёте читается
    // заново через get_block_world(pos), так что если блок под источником вдруг изменился
    // (не должно происходить штатно, но на всякий случай) — пересчёт сам всё поправит.
    // Хранятся ПО ЧАНКАМ (ключ — координаты чанка, значение — мировые позиции его источников):
    // локальному пересчёту света нужны только источники в окрестности правки, а не весь список мира,
    // и при загрузке чанка список его источников просто заменяется — без поиска дубликатов.
    // Запись удаляется при выгрузке чанка и восстанавливается сканом при следующей загрузке.
    std::unordered_map<glm::ivec3, std::vector<glm::ivec3>, Chunk_Key_Hash> m_light_sources;
    // Чанки, пришедшие с пустой сеткой блочного света: свет соседей и их собственных источников
    // дотягивается до них одним инкрементальным проходом (relight_block_light_new_chunks).
    std::vector<glm::ivec3> m_chunks_pending_block_light;
    void add_light_source(const glm::ivec3& world_pos);
    void remove_light_source(const glm::ivec3& world_pos);

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
    void enqueue_chunk_column_load(int cx, int cz, const std::vector<Chunk_Job>& chunks);
    // Приоритет задачи генерации: квадрат расстояния до игрока, вертикаль весит в 4 раза сильнее.
    int chunk_job_priority(int cx, int cy, int cz) const;
    // Лимит одновременно запрошенных чанков: растёт со скоростью игрока.
    size_t max_inflight_chunks() const;
    // Пересчитывает вертикальное окно по скорости и дальности; true, если оно изменилось.
    bool update_vertical_window();
    // Пересчёт приоритетов всех задач в очереди после смены чанка игроком; задачи вне окна загрузки
    // снимаются (их запись в m_requested_chunks тоже чистится, чтобы чанк можно было поставить снова).
    void reprioritize_pending_jobs();
    // То же для задач мешинга: приоритет пересчитывается по текущему кольцу и расстоянию (задачи не снимаются).
    void reprioritize_pending_meshes();
    bool save_chunk_to_disk(const Chunk& chunk) const;
    void enqueue_mesh_rebuild(int cx, int cy, int cz, bool urgent = false);
    bool is_chunk_wanted(const glm::ivec3& position) const;
    void start_workers();
    void disk_reader_loop();
    void requeue_generation(const Chunk_Job& job);
    void enqueue_generation_column(Chunk_Column_Job column);
    void schedule_region_preparation_locked(int chunk_x, int chunk_z, uint64_t generation_id,
                                            int priority, int64_t region_key, uint8_t missing_parts);
    // Starts cave/hydrology region work before the player reaches a region boundary.
    // These are background jobs; no chunk worker waits on a cold region future.
    void prewarm_generation_regions_near_player();
    void stop_workers();
    void worker_loop(bool mesh_dedicated = false);

    bool set_light_world(int wx, int wy, int wz, uint8_t level);
    // Локальный пересчёт блочного света после правки блока в world_pos: очищается куб радиуса
    // Config::max_light_level вокруг правки (дальше правка свет изменить не может) и заполняется
    // заново от источников внутри куба и от неизменённых клеток сразу за его границей. Меши
    // пересобираются только у чанков, где свет РЕАЛЬНО поменялся.
    void relight_block_light_near(const glm::ivec3& world_pos);
    // Инкрементальный пересчёт для только что загруженных чанков (m_chunks_pending_block_light):
    // свет только прибавляется, очистка не нужна.
    void relight_block_light_new_chunks();
    // Локальный пересчёт света неба после правки: вертикальная тень пересчитывается только для
    // колонки правки, горизонтальное растекание — в кубе вокруг неё (см. реализацию).
    void relight_sky_light_near(const glm::ivec3& world_pos);
    // Грани соседа читают свет через границу: после смены света в чанках из changed_chunks
    // шесть соседей помечаются грязными, иначе их граница держала бы старый свет.
    void mark_light_neighbors_dirty(const std::unordered_set<glm::ivec3, Chunk_Key_Hash>& changed_chunks);
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
    // Генератор мира (для запросов биома, например спавном мобов); nullptr, если не задан.
    const World_Generator* world_generator() const { return m_world_generator; }
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
        int flora_render_distance_chunks = -1,
        bool spectator = false
    ) const;
    std::vector<glm::ivec3> take_unloaded_chunk_positions();

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

    // Для отладочного оверлея (F3+F4): сколько чанков сейчас в работе.
    struct Chunk_Work_Stats {
        size_t mesh_pending = 0;     // чанки, ждущие пересборки меша
        size_t mesh_requeued = 0;    // пересборки, отложенные до готовности соседей
        size_t loads_requested = 0;  // чанки, запрошенные у генератора/диска, но ещё не пришедшие
    };
    Chunk_Work_Stats get_work_stats() const {
        return {m_pending_mesh_rebuilds.size(), m_mesh_rebuild_requeue.size(), m_requested_chunks.size()};
    }
};

#endif //OPTICRAFT_CHUNK_MANAGER_H
