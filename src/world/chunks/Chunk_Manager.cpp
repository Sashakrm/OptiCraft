//
// Created by noktemor on 28.03.2026.
//


#include "world/chunks/Chunk_Manager.h"
#include "world/crafting/Smelting.h"
#include "utils/logging/Logger.h"
#include "utils/Config.h"
#include "math/Math_Helpers.h"
#include "rendering/atlas/Texture_Atlas.h"
#include "rendering/Frustum.h"
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <chrono>

namespace {
    constexpr size_t max_finished_chunks_per_frame = 4;
    constexpr size_t max_finished_meshes_per_frame = 4;
    constexpr size_t max_mesh_jobs_enqueued_per_frame = 16;
    // Вертикальное окно: 8 слоёв обычно, 4 при быстром движении по Y или малой дальности.
    constexpr int kVerticalHalfNormal = 4;
    constexpr int kVerticalHalfReduced = 2;
    constexpr float kFastVerticalSpeed = 6.0f;   // блоков/с: выше — окно сжимается
    constexpr float kSlowVerticalSpeed = 3.0f;   // блоков/с: ниже — окно возвращается (гистерезис)
    constexpr int kSmallRenderRadius = 4;        // при такой дальности окно всегда сжато
    // Лимит очереди запросов: база + по одному на блок/с скорости.
    constexpr size_t kBaseInflightChunks = 128;
    constexpr float kSpeedFullBias = 40.0f;      // блоков/с, при которых смещение к движению максимально
    // Режим захвата области: мир стоит на паузе, поэтому за кадр можно принять на порядок больше.
    constexpr size_t capture_max_finished_chunks_per_frame = 256;
    constexpr size_t capture_max_finished_meshes_per_frame = 128;
    constexpr size_t capture_max_mesh_jobs_enqueued_per_frame = 256;
    constexpr size_t capture_sky_columns_per_frame = 48;
    constexpr double capture_plan_budget_ms = 8.0;

    int floor_div_chunk(int value, int divisor) {
        int q = value / divisor;
        if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) --q;
        return q;
    }

    // --- Освещение: общие быстрые примитивы для recompute_lighting и recompute_sky_lighting ---
    //
    // Раньше оба BFS носили в узле очереди только мировые координаты (wx,wy,wz). Чтобы
    // прочитать/записать свет соседа, код КАЖДЫЙ раз проделывал: get_chunk_coords_for_world
    // (три целочисленных деления, Math_Helpers::floor_div) -> Chunk_Manager::get_chunk
    // (поиск в unordered_map<glm::ivec3, ...> — хэш + пробинг) -> вычесть chunk*size, чтобы
    // получить локальные координаты. И так на КАЖДОГО из 6 соседей КАЖДОГО узла очереди —
    // то есть на каждый распространённый воксель до шести обращений к hash-map и восемнадцать
    // делений. Voxel_Cursor ниже хранит уже разложенные (chunk-координаты + локальные
    // координаты + указатель на сам Chunk), а step_cursor двигает его на ±1 по одной оси
    // чистым сложением/вычитанием и лезет в get_chunk (hash-map) ТОЛЬКО когда локальная
    // координата реально вышла за границу чанка. Так как offsets всегда меняют ровно одну
    // ось на ±1, пересечь можно не больше одной границы чанка за шаг. На практике на границу
    // чанка попадает только внешний слой вокселей (6*32*32 из 32768 = ~19%), так что для
    // подавляющего большинства шагов BFS вообще не трогает unordered_map.
    struct Voxel_Cursor {
        int cx, cy, cz;    // координаты чанка
        int lx, ly, lz;    // локальные координаты внутри чанка, [0, chunk_size)/[0, chunk_height)
        Chunk* chunk;      // закэшированный указатель на этот чанк (может быть nullptr)
    };

    inline Voxel_Cursor step_cursor(const Voxel_Cursor& from, int dx, int dy, int dz, Chunk_Manager& mgr) {
        Voxel_Cursor to = from;
        to.lx += dx;
        to.ly += dy;
        to.lz += dz;

        bool crossed = false;
        if (to.lx < 0)                          { to.lx += Config::chunk_size;   --to.cx; crossed = true; }
        else if (to.lx >= Config::chunk_size)   { to.lx -= Config::chunk_size;   ++to.cx; crossed = true; }
        if (to.ly < 0)                          { to.ly += Config::chunk_height; --to.cy; crossed = true; }
        else if (to.ly >= Config::chunk_height) { to.ly -= Config::chunk_height; ++to.cy; crossed = true; }
        if (to.lz < 0)                          { to.lz += Config::chunk_size;   --to.cz; crossed = true; }
        else if (to.lz >= Config::chunk_size)   { to.lz -= Config::chunk_size;   ++to.cz; crossed = true; }

        // Остались в том же чанке — переиспользуем указатель без повторного get_chunk().
        to.chunk = crossed ? mgr.get_chunk(to.cx, to.cy, to.cz) : from.chunk;
        return to;
    }

    // Плоская очередь на замену std::queue<T> (у которого контейнер по умолчанию —
    // std::deque<T>: данные хранятся не одним куском, а цепочкой отдельно аллоцируемых
    // "чанков" фиксированного размера, и каждый push/pop — это лишний уровень косвенности
    // плюс рваная для кэша память). Flat_Queue — один непрерывный std::vector<T> с курсором
    // head: push — обычный push_back, pop — просто ++head, ничего не освобождается и не
    // двигается. Для BFS, который и так идёт "уровень за уровнем" (level неубывающе
    // уменьшается по FIFO), это даёт строго последовательный проход по памяти.
    template <typename T>
    struct Flat_Queue {
        std::vector<T> data;
        size_t head = 0;

        void push(const T& v) { data.push_back(v); }
        const T& front() const { return data[head]; }
        void pop() { ++head; }
        bool empty() const { return head >= data.size(); }
    };

    static constexpr int k_dx6[6] = {1, -1, 0, 0, 0, 0};
    static constexpr int k_dy6[6] = {0, 0, 1, -1, 0, 0};
    static constexpr int k_dz6[6] = {0, 0, 0, 0, 1, -1};
}

Chunk_Manager::Chunk_Manager(int load_radius)
    : m_load_radius(load_radius)
{
    // Слотов хватает на максимальную прорисовку в полном вертикальном окне плюс запас на задачи в
    // полёте и выгрузки. Пул не растёт: при исчерпании загрузка просто откладывается.
    constexpr size_t kPoolHeadroom = 2048;
    const int radius = std::max(load_radius, Config::render_distance_max);
    const size_t side = static_cast<size_t>(2 * radius + 1);
    const size_t capacity = side * side * static_cast<size_t>(2 * (kVerticalHalfNormal + 1) + 1) + kPoolHeadroom;
    m_chunk_pool = std::make_unique<Chunk_Slot_Pool>(capacity);
    m_chunk_io = std::make_unique<Chunk_IO>();

    start_workers();
}

Chunk_Manager::~Chunk_Manager() {
    stop_workers();
    // Воркеры уже остановлены, поэтому m_chunks трогает только этот поток.
    save_all();
}

void Chunk_Manager::save_all() {
    m_chunks.for_each([this](const glm::ivec3&, const Chunk_Ptr& chunk) {
        save_chunk_to_disk(*chunk);
    });
    // Дописываем очередь записи до конца, и только потом сбрасываем индексы регионов.
    m_chunk_io->drain();
    m_world_file.flush_chunks(m_generation_folder);
    Block_Entity_Store::save(get_block_entities_path(), m_block_entities);
}

void Chunk_Manager::set_world_storage(const World_File& world_file, const std::string& generation_folder) {
    // Пресет генерации реально меняется (см. Game::switch_generation_type) — сохраняем
    // содержимое контейнеров ПРЕДЫДУЩЕГО пресета, прежде чем переключиться на новый набор,
    // иначе то, что игрок успел положить в сундук, молча терялось бы при возврате назад.
    if (!m_generation_folder.empty() && m_generation_folder != generation_folder) {
        Block_Entity_Store::save(get_block_entities_path(), m_block_entities);
    }

    m_world_file = world_file;
    m_generation_folder = generation_folder;

    Block_Entity_Store::load(get_block_entities_path(), m_block_entities);
}

std::filesystem::path Chunk_Manager::get_block_entities_path() const {
    std::filesystem::path dir = m_world_file.get_path();
    dir.remove_filename();
    return dir / ("block_entities_" + m_generation_folder + ".json");
}

Block_Entity* Chunk_Manager::get_block_entity(int wx, int wy, int wz) {
    const auto it = m_block_entities.find(glm::ivec3{wx, wy, wz});
    return it == m_block_entities.end() ? nullptr : &it->second;
}

const Block_Entity* Chunk_Manager::get_block_entity(int wx, int wy, int wz) const {
    const auto it = m_block_entities.find(glm::ivec3{wx, wy, wz});
    return it == m_block_entities.end() ? nullptr : &it->second;
}

Block_Entity* Chunk_Manager::get_paired_block_entity(int wx, int wy, int wz) {
    Block_Entity* entity = get_block_entity(wx, wy, wz);
    if (!entity || !entity->has_pair()) return nullptr;
    return get_block_entity(wx + entity->paired_dx, wy, wz + entity->paired_dz);
}

void Chunk_Manager::sync_block_entity(int wx, int wy, int wz, Block_Types old_type, Block_Types new_type) {
    const glm::ivec3 world_pos{wx, wy, wz};
    const Block_Entity_Type old_entity_type = block_entity_type_for(old_type);
    const Block_Entity_Type new_entity_type = block_entity_type_for(new_type);

    if (old_entity_type == new_entity_type) {
        return; // сняли и тут же поставили тот же тип контейнера (или вообще не менялось) — не трогаем содержимое
    }

    if (old_entity_type != Block_Entity_Type::None) {
        // Блок-контейнер сломан/заменён на что-то другое — если он был половиной двойного
        // сундука, сперва разрываем связь со второй половиной, чтобы она не ссылалась в
        // никуда, иначе get_paired_block_entity молча начал бы читать чужую позицию.
        const auto it = m_block_entities.find(world_pos);
        if (it != m_block_entities.end() && it->second.has_pair()) {
            const glm::ivec3 pair_pos{wx + it->second.paired_dx, wy, wz + it->second.paired_dz};
            const auto pair_it = m_block_entities.find(pair_pos);
            if (pair_it != m_block_entities.end()) {
                pair_it->second.paired_dx = 0;
                pair_it->second.paired_dz = 0;
            }
        }
        // Содержимое здесь только стирается вместе с блоком: выбросить его на землю должен
        // тот, кто ломает блок (Block_Interaction забирает слоты ДО set_block_world и передаёт
        // их в Dropped_Item_Manager::spawn_block_drops).
        m_block_entities.erase(world_pos);
    }

    if (new_entity_type != Block_Entity_Type::None) {
        Block_Entity entity = Block_Entity::make(new_entity_type);

        // Сундук впритык к другому одиночному сундуку на той же высоте объединяется в
        // двойной — как в Minecraft. Проверяем только 4 горизонтальных соседа, только
        // если это сундук, и только если сосед ещё НЕ связан с кем-то третьим (иначе
        // получилась бы тройная цепочка, которой Minecraft не поддерживает, и мы тоже нет).
        if (new_entity_type == Block_Entity_Type::Chest) {
            static constexpr int k_dx4[4] = {1, -1, 0, 0};
            static constexpr int k_dz4[4] = {0, 0, 1, -1};
            for (int i = 0; i < 4; ++i) {
                const int nx = wx + k_dx4[i];
                const int nz = wz + k_dz4[i];
                const auto neighbor_it = m_block_entities.find(glm::ivec3{nx, wy, nz});
                if (neighbor_it == m_block_entities.end()) continue;
                if (neighbor_it->second.type != Block_Entity_Type::Chest) continue;
                if (neighbor_it->second.has_pair()) continue;

                entity.paired_dx = static_cast<int8_t>(k_dx4[i]);
                entity.paired_dz = static_cast<int8_t>(k_dz4[i]);
                neighbor_it->second.paired_dx = static_cast<int8_t>(-k_dx4[i]);
                neighbor_it->second.paired_dz = static_cast<int8_t>(-k_dz4[i]);
                break;
            }
        }

        m_block_entities[world_pos] = std::move(entity);
    }
}

void Chunk_Manager::update_fluids(float delta_time) {
    m_fluids.update(*this, delta_time);
}

void Chunk_Manager::update_block_entities(float delta_time) {
    const Smelting_Registry& smelting = Smelting_Registry::get_instance();
    constexpr int k_max_stack = 64;

    for (auto& [pos, entity] : m_block_entities) {
        if (entity.type != Block_Entity_Type::Furnace) continue;

        const glm::ivec3 chunk = get_chunk_coords_for_world(pos.x, pos.y, pos.z);
        if (!is_chunk_loaded(chunk.x, chunk.y, chunk.z)) continue;

        Item_Stack& input = entity.slots[Block_Entity_Slots::furnace_input];
        Item_Stack& fuel = entity.slots[Block_Entity_Slots::furnace_fuel];
        Item_Stack& output = entity.slots[Block_Entity_Slots::furnace_output];

        const Smelting_Recipe* recipe = input.is_empty() ? nullptr : smelting.find(input.type);
        const bool can_smelt = recipe &&
            (output.is_empty() ||
             (output.type == recipe->output && output.count + recipe->output_count <= k_max_stack));

        // Топливо сгорает только когда есть что жарить — как в Minecraft, пустая печь не тратит уголь.
        if (!entity.is_burning() && can_smelt && !fuel.is_empty()) {
            const float fuel_time = smelting.get_fuel_time(fuel.type);
            if (fuel_time > 0.0f) {
                entity.burn_time_left = fuel_time;
                entity.burn_time_total = fuel_time;
                fuel.count -= 1;
                if (fuel.count <= 0) fuel = Item_Stack{};
            }
        }

        if (entity.is_burning()) {
            entity.burn_time_left = std::max(0.0f, entity.burn_time_left - delta_time);
            if (can_smelt) {
                entity.cook_progress += delta_time;
                if (entity.cook_progress >= recipe->time_seconds) {
                    entity.cook_progress = 0.0f;
                    if (output.is_empty()) {
                        output = Item_Stack{recipe->output, recipe->output_count};
                    } else {
                        output.count += recipe->output_count;
                    }
                    input.count -= 1;
                    if (input.count <= 0) input = Item_Stack{};
                }
            } else {
                entity.cook_progress = 0.0f;
            }
        } else if (entity.cook_progress > 0.0f) {
            // Печь погасла или вход убрали — прожарка остывает вдвое быстрее, чем росла.
            entity.cook_progress = std::max(0.0f, entity.cook_progress - delta_time * 2.0f);
        }

        if (!entity.is_burning()) entity.burn_time_total = 0.0f;

        // Активная текстура печи: бит "активен" в метаданных блока следует за горением.
        // Пишем только при смене — set_block_meta_world перестраивает меш чанка.
        const uint8_t meta = get_block_meta_world(pos.x, pos.y, pos.z);
        const bool active_now = (meta & Block_Meta::active_bit) != 0;
        if (active_now != entity.is_burning()) {
            const uint8_t new_meta = entity.is_burning()
                ? static_cast<uint8_t>(meta | Block_Meta::active_bit)
                : static_cast<uint8_t>(meta & ~Block_Meta::active_bit);
            set_block_meta_world(pos.x, pos.y, pos.z, new_meta);
        }
    }
}

void Chunk_Manager::start_workers() {
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    // Не создаём поток на каждое логическое ядро: при большом hardware_concurrency
    // это приводит к одновременной генерации слишком большого числа чанков и
    // исчерпанию памяти ещё до того, как очередь загрузки успеет стабилизироваться.
    const unsigned int available_workers = hardware_threads > 2 ? hardware_threads - 2 : 1u;
    const unsigned int worker_count = std::max(2u, std::min(6u, available_workers));

    m_workers.reserve(worker_count);
    m_disk_reader = std::thread(&Chunk_Manager::disk_reader_loop, this);
    // Один поток всегда оставляем свободным именно под меши. При длинной очереди
    // генерации это гарантирует, что редактирование блока не ждёт завершения
    // процедурной генерации десятков соседних чанков.
    for (unsigned int i = 0; i < worker_count; ++i) {
        const bool mesh_dedicated = (i == worker_count - 1);
        m_workers.emplace_back(&Chunk_Manager::worker_loop, this, mesh_dedicated);
    }
}

void Chunk_Manager::stop_workers() {
    {
        std::lock_guard<std::mutex> lock(m_job_mutex);
        m_stop_workers = true;
        m_pending_jobs.clear();
        m_pending_job_count.store(0);
        m_pending_mesh_jobs.clear();
        m_pending_mesh_count.store(0);
        m_pending_disk_reads.clear();
    }
    m_job_available.notify_all();

    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_workers.clear();
    if (m_disk_reader.joinable()) {
        m_disk_reader.join();
    }
}

void Chunk_Manager::requeue_generation(const Chunk_Job& job) {
    m_pending_jobs.push(job, job.priority);
    m_pending_job_count.fetch_add(1);
    wake_workers();
}

void Chunk_Manager::wake_workers() {
    // Будим только если кто-то реально спит: иначе производитель не трогает мьютекс вовсе.
    // Порядок seq_cst: либо спящий увидит новую задачу в предикате, либо мы увидим его в m_sleepers.
    if (m_sleepers.load() > 0) {
        { std::lock_guard<std::mutex> lock(m_job_mutex); }
        m_job_available.notify_all();
    }
}

void Chunk_Manager::disk_reader_loop() {
    // Пачка за раз: все чанки одного региона читаются за один проход, в порядке расположения на диске.
    constexpr size_t kReadBatch = 64;
    while (true) {
        std::vector<Chunk_Job> batch;
        const World_Generator* generator = nullptr;
        const Texture_Atlas* atlas = nullptr;
        World_File world_file;
        std::string folder;
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(m_job_mutex);
            m_job_available.wait(lock, [this] { return m_stop_workers || !m_pending_disk_reads.empty(); });
            if (m_stop_workers) return;
            while (!m_pending_disk_reads.empty() && batch.size() < kReadBatch) {
                batch.push_back(m_pending_disk_reads.front());
                m_pending_disk_reads.pop_front();
            }
            // Снимок мира берём под тем же мьютексом, что и задачу (см. worker_loop).
            generator = m_world_generator;
            atlas = m_texture_atlas;
            world_file = m_world_file;
            folder = m_generation_folder;
            generation = m_generation_id;
        }

        const auto region = Region_File::get(world_file.get_region_path(folder));
        std::vector<Region_File::Key> keys;
        std::vector<size_t> owners;   // индекс задачи в batch для каждого ключа
        for (size_t i = 0; i < batch.size(); ++i) {
            if (batch[i].generation_id != generation || !region || !generator) continue;
            keys.push_back({batch[i].position.x, batch[i].position.y, batch[i].position.z});
            owners.push_back(i);
        }

        std::vector<uint32_t> stages;
        if (!keys.empty()) m_chunk_io->read_batch(region, keys, stages);

        // Задачи, которые не удалось прочитать или построить, уходят генератору; устаревшие — отбрасываются.
        std::vector<bool> handled(batch.size(), false);
        for (size_t k = 0; k < keys.size(); ++k) {
            const Chunk_Job& job = batch[owners[k]];
            handled[owners[k]] = true;
            if (stages[k] == Chunk_IO::kNone) {
                requeue_generation(job);
                continue;
            }

            Chunk_Ptr chunk(m_chunk_pool->acquire(job.position.x, job.position.y, job.position.z, *generator),
                            Chunk_Slot_Release{m_chunk_pool.get()});
            if (!chunk) {
                m_chunk_io->release_stage(stages[k]);
                requeue_generation(job);
                continue;
            }
            chunk->get_blocks_mutable() = m_chunk_io->stage(stages[k]);
            m_chunk_io->release_stage(stages[k]);
            chunk->recalculate_metadata();
            chunk->clear_modified();
            chunk->set_texture_atlas(atlas);

            m_completed_jobs.push({job.position, job.generation_id, std::move(chunk)});
        }
        // Задачи без ключа (старый мир или нет региона): пусть решает генератор, он же проверит легаси-файлы.
        for (size_t i = 0; i < batch.size(); ++i) {
            if (!handled[i] && batch[i].generation_id == generation) requeue_generation(batch[i]);
        }
    }
}

void Chunk_Manager::worker_loop(bool mesh_dedicated) {
    while (true) {
        Chunk_Job job;
        Mesh_Job mesh_job;
        bool has_generation_job = false;
        bool has_mesh_job = false;
        const World_Generator* generator = nullptr;
        World_File world_file;
        std::string generation_folder;
        {
            std::unique_lock<std::mutex> lock(m_job_mutex);
            m_sleepers.fetch_add(1);
            m_job_available.wait(lock, [this, mesh_dedicated] {
                if (m_stop_workers) return true;
                if (m_pending_mesh_count.load() > 0) return true;
                return !mesh_dedicated && m_pending_job_count.load() > 0;
            });
            m_sleepers.fetch_sub(1);

            if (m_stop_workers) {
                return;
            }

            // Меши всегда имеют абсолютный приоритет над генерацией. Это критично для
            // интерактивности: даже если вокруг ещё строится огромный вертикальный мир,
            // ломание/постановка блока должно получить новый меш как можно быстрее.
        }

        if (m_pending_mesh_count.load() > 0) {
            std::unique_ptr<Mesh_Job> pending;
            if (m_pending_mesh_jobs.try_pop(pending)) {
                mesh_job = std::move(*pending);
                m_pending_mesh_count.fetch_sub(1);
                has_mesh_job = true;
            }
        }

        if (!has_mesh_job) {
            // Задачи генерации берём без замка: корзины по приоритету.
            if (mesh_dedicated || !m_pending_jobs.try_pop(job)) {
                continue;
            }
            m_pending_job_count.fetch_sub(1);
        }

        // Снимок состояния мира берётся под тем же мьютексом, что и задача: главный поток может
        // сменить мир (clear/set_world_storage), и воркер не должен записать чанк в чужой регион.
        {
            std::lock_guard<std::mutex> lock(m_job_mutex);
            if (!has_mesh_job) {
                // Задача от прошлого мира (до clear/смены мира) не должна ни генерироваться, ни писаться.
                has_generation_job = job.generation_id == m_generation_id;
            }
            generator = m_world_generator;
            world_file = m_world_file;
            generation_folder = m_generation_folder;
        }

        if (has_generation_job) {
            if (!generator) {
                continue;
            }

            Chunk_Ptr chunk(m_chunk_pool->acquire(job.position.x, job.position.y, job.position.z, *generator),
                            Chunk_Slot_Release{m_chunk_pool.get()});
            if (!chunk) {
                // Пул слотов исчерпан: результат без чанка. Главный поток снимет «запрошено», и загрузка
                // повторится на следующем кадре, если чанк всё ещё нужен.
                m_completed_jobs.push({job.position, job.generation_id, nullptr});
                continue;
            }
            chunk->set_texture_atlas(m_texture_atlas);

            // Persistent-world rule: an existing chunk file ALWAYS wins over procedural
            // generation. A missing file means this is genuinely new terrain.
            // Логи убраны намеренно: это тысячи std::endl (каждый со сбросом
            // потока) из нескольких воркеров сразу — и лишняя работа, и точка
            // конкуренции на std::cout при массовой загрузке мира.
            if (world_file.load_chunk(job.position.x, job.position.y, job.position.z,
                                      generation_folder,
                                      chunk->get_blocks_mutable())) {
                chunk->recalculate_metadata();
                // Чанк только что прочитан с диска — он уже сохранён.
                chunk->clear_modified();
            } else {
                chunk->generate_blocks();
                // Каждый сгенерированный чанк сразу пишется на диск. При быстром перемещении
                // (игрок проходит десятки блоков в секунду) повторная генерация дороже чтения,
                // а чанк, который уже лежит на диске, при следующей загрузке читается без генерации.
                // Чанк ещё не передан главному потоку, поэтому запись из воркера безопасна.
                const Region_File::Key key{job.position.x, job.position.y, job.position.z};
                const auto region = Region_File::get(world_file.get_region_path(generation_folder));
                if (!region || !m_chunk_io->enqueue_write(region, key, chunk->get_blocks())) {
                    if (!world_file.save_chunk(job.position.x, job.position.y, job.position.z,
                                               generation_folder, chunk->get_blocks())) {
                        LOG_WARN("Cannot save generated chunk " + std::to_string(job.position.x) + " " +
                                 std::to_string(job.position.y) + " " + std::to_string(job.position.z));
                    }
                }
            }
            m_completed_jobs.push({job.position, job.generation_id, std::move(chunk)});
        } else if (has_mesh_job) {
            if (!m_texture_atlas) {
                continue;
            }

            // Блок за пределами чанка берётся из снимков, сделанных на главном потоке (см. enqueue_mesh_rebuild):
            // клетку, вышедшую за одну ось, отдаёт грань соседа, а клетку у ребра или угла — кольцо. AO читает
            // диагональных соседей, поэтому шести граней для меша уже недостаточно.
            const Mesh_Job& snapshot = mesh_job;
            Chunk_Neighbor_Lookup lookup = [&snapshot, generator](int wx, int wy, int wz) -> Block_Types {
                const int local_x = wx - snapshot.position.x * Config::chunk_size;
                const int local_y = wy - snapshot.position.y * Config::chunk_height;
                const int local_z = wz - snapshot.position.z * Config::chunk_size;

                int neighbor_index = -1;
                if (local_x < 0) neighbor_index = 0;
                else if (local_x >= Config::chunk_size) neighbor_index = 1;
                else if (local_y < 0) neighbor_index = 2;
                else if (local_y >= Config::chunk_height) neighbor_index = 3;
                else if (local_z < 0) neighbor_index = 4;
                else if (local_z >= Config::chunk_size) neighbor_index = 5;

                // Рёбра и углы (две или три координаты вне чанка) берутся только из кольца снимка. Грань
                // соседа для них не подходит: клетка лежит в диагональном чанке, а грань отдала бы
                // чужую клетку (вторая координата обёрнута по модулю чанка). Если кольца нет (диагональный
                // чанк не был загружен при постановке), идём сразу к фолбэку генератора.
                const int out_axes = int(local_x < 0 || local_x >= Config::chunk_size) +
                                     int(local_y < 0 || local_y >= Config::chunk_height) +
                                     int(local_z < 0 || local_z >= Config::chunk_size);
                if (out_axes >= 2) {
                    const int ring = chunk_ring_index(local_x, local_y, local_z);
                    if (ring >= 0 && snapshot.ring_valid[ring]) {
                        return static_cast<Block_Types>(snapshot.ring_blocks[ring]);
                    }
                } else if (neighbor_index >= 0 && snapshot.neighbor_blocks[neighbor_index]) {
                    const int nlx = ((local_x % Config::chunk_size) + Config::chunk_size) % Config::chunk_size;
                    const int nly = ((local_y % Config::chunk_height) + Config::chunk_height) % Config::chunk_height;
                    const int nlz = ((local_z % Config::chunk_size) + Config::chunk_size) % Config::chunk_size;
                    const auto& face = *snapshot.neighbor_blocks[neighbor_index];
                    // -X/+X faces are indexed by (z,y), -Y/+Y by (x,z), -Z/+Z by (x,y).
                    if (neighbor_index == 0 || neighbor_index == 1) return face.get(nlz, nly);
                    if (neighbor_index == 2 || neighbor_index == 3) return face.get(nlx, nlz);
                    return face.get(nlx, nly);
                }

                // Область захвата: за её горизонтальными границами мира «нет» — воздух, а не
                // продолжение рельефа от генератора (иначе у края кадра не будет боковых граней).
                if (snapshot.clip.active) {
                    const int ncx = floor_div_chunk(wx, Config::chunk_size);
                    const int ncz = floor_div_chunk(wz, Config::chunk_size);
                    if (ncx < snapshot.clip.min_cx || ncx > snapshot.clip.max_cx ||
                        ncz < snapshot.clip.min_cz || ncz > snapshot.clip.max_cz) {
                        return Block_Types::Air;
                    }
                }

                return generator ? generator->get_block(wx, wy, wz) : Block_Types::Air;
            };

            // Тот же принцип поиска соседа, но для света — фолбэк 0 (не World_Generator,
            // у света нет "процедурного" источника за пределами загруженного мира).
            Chunk_Light_Lookup light_lookup = [&snapshot](int wx, int wy, int wz) -> uint8_t {
                const int local_x = wx - snapshot.position.x * Config::chunk_size;
                const int local_y = wy - snapshot.position.y * Config::chunk_height;
                const int local_z = wz - snapshot.position.z * Config::chunk_size;

                int neighbor_index = -1;
                if (local_x < 0) neighbor_index = 0;
                else if (local_x >= Config::chunk_size) neighbor_index = 1;
                else if (local_y < 0) neighbor_index = 2;
                else if (local_y >= Config::chunk_height) neighbor_index = 3;
                else if (local_z < 0) neighbor_index = 4;
                else if (local_z >= Config::chunk_size) neighbor_index = 5;

                if (neighbor_index >= 0 && snapshot.neighbor_light[neighbor_index]) {
                    const int nlx = ((local_x % Config::chunk_size) + Config::chunk_size) % Config::chunk_size;
                    const int nly = ((local_y % Config::chunk_height) + Config::chunk_height) % Config::chunk_height;
                    const int nlz = ((local_z % Config::chunk_size) + Config::chunk_size) % Config::chunk_size;
                    const auto& face = *snapshot.neighbor_light[neighbor_index];
                    if (neighbor_index == 0 || neighbor_index == 1) return face.get(nlz, nly);
                    if (neighbor_index == 2 || neighbor_index == 3) return face.get(nlx, nlz);
                    return face.get(nlx, nly);
                }

                // За границей области захвата — открытое небо (небесный свет 15 в старшем
                // ниббле), чтобы боковые грани среза не были чёрными.
                if (snapshot.clip.active) {
                    const int ncx = floor_div_chunk(wx, Config::chunk_size);
                    const int ncz = floor_div_chunk(wz, Config::chunk_size);
                    if (ncx < snapshot.clip.min_cx || ncx > snapshot.clip.max_cx ||
                        ncz < snapshot.clip.min_cz || ncz > snapshot.clip.max_cz) {
                        return 0xF0;
                    }
                }

                // Над верхней границей загруженного столба небо открыто (та же логика, что у
                // прямого света в recompute_sky_lighting): иначе грани у края окна темнеют.
                if (neighbor_index == 3) return 0xF0;

                return 0;
            };

            // Метаданные соседа (уровень жидкости) — из тех же снимков граней. Нет снимка — 0
            // (источник), как и у моря, которое достраивает генератор.
            Chunk_Meta_Lookup meta_lookup = [&snapshot](int wx, int wy, int wz) -> uint8_t {
                const int local_x = wx - snapshot.position.x * Config::chunk_size;
                const int local_y = wy - snapshot.position.y * Config::chunk_height;
                const int local_z = wz - snapshot.position.z * Config::chunk_size;

                int neighbor_index = -1;
                if (local_x < 0) neighbor_index = 0;
                else if (local_x >= Config::chunk_size) neighbor_index = 1;
                else if (local_y < 0) neighbor_index = 2;
                else if (local_y >= Config::chunk_height) neighbor_index = 3;
                else if (local_z < 0) neighbor_index = 4;
                else if (local_z >= Config::chunk_size) neighbor_index = 5;

                if (neighbor_index >= 0 && snapshot.neighbor_blocks[neighbor_index]) {
                    const int nlx = ((local_x % Config::chunk_size) + Config::chunk_size) % Config::chunk_size;
                    const int nly = ((local_y % Config::chunk_height) + Config::chunk_height) % Config::chunk_height;
                    const int nlz = ((local_z % Config::chunk_size) + Config::chunk_size) % Config::chunk_size;
                    const auto& face = *snapshot.neighbor_blocks[neighbor_index];
                    if (neighbor_index == 0 || neighbor_index == 1) return face.get_meta(nlz, nly);
                    if (neighbor_index == 2 || neighbor_index == 3) return face.get_meta(nlx, nlz);
                    return face.get_meta(nlx, nly);
                }
                return 0;
            };

            // Всё тяжёлое (грани, AO, свет, высоты воды) считает видеокарта; воркер лишь готовит
            // связность секций и упаковывает блоки, метаданные и свет в палитровые каналы.
            Chunk_Meshes meshes = build_chunk_gpu_mesh(mesh_job.own_blocks, mesh_job.own_light,
                                                       mesh_job.neighbor_blocks, mesh_job.neighbor_light,
                                                       mesh_job.position.x, mesh_job.position.y, mesh_job.position.z,
                                                       lookup, light_lookup, meta_lookup, mesh_job.clip.active);

            Mesh_Result done{mesh_job.position, mesh_job.generation_id, std::move(meshes),
                             mesh_job.dirty_epoch, mesh_job.instance_id, mesh_job.seq, mesh_job.urgent};
            if (mesh_job.urgent) m_completed_urgent_mesh_jobs.push(std::move(done));
            else m_completed_mesh_jobs.push(std::move(done));
        }
    }
}

bool Chunk_Manager::is_chunk_loaded(int cx, int cy, int cz) const {
    return m_chunks.contains(cx, cy, cz);
}

bool Chunk_Manager::save_chunk_to_disk(const Chunk& chunk) const {
    // Нетронутый чанк детерминированно восстанавливается генератором по сиду,
    // поэтому записывать его незачем. Раньше выгрузка сохраняла подряд все
    // чанки, и обычная ходьба по миру превращалась в поток записей на диск.
    if (!chunk.is_modified()) return true;

    // Главный поток не ждёт диск: блоки копируются в буфер, а пачку пишет писатель Chunk_IO.
    const Region_File::Key key{chunk.get_chunk_x(), chunk.get_chunk_y(), chunk.get_chunk_z()};
    const auto region = Region_File::get(m_world_file.get_region_path(m_generation_folder));
    if (region && m_chunk_io->enqueue_write(region, key, chunk.get_blocks())) return true;

    // Буферов нет: дожидаемся очереди, чтобы более старая запись не перезаписала эту, и пишем синхронно.
    m_chunk_io->drain();
    return m_world_file.save_chunk(chunk.get_chunk_x(), chunk.get_chunk_y(), chunk.get_chunk_z(),
                                   m_generation_folder, chunk.get_blocks());
}

void Chunk_Manager::unload_chunk(int cx, int cy, int cz) {
    if (Chunk* existing = m_chunks.find(cx, cy, cz)) {
        save_chunk_to_disk(*existing);
        m_unloaded_since_last_query.push_back({cx, cy, cz});
        m_chunks.erase(cx, cy, cz);
        ++m_chunk_set_epoch;
    }
    m_requested_chunks.erase(cx, cy, cz);
    // Результат задачи, снятой для этого объекта, collect_finished_mesh_jobs отбросит по instance_id.
    // Чистка здесь нужна для состояния позиции: иначе, если на ней скоро появится новый чанк,
    // enqueue_mesh_rebuild решит, что для него уже что-то летит, и молча пропустит постановку задачи.
    m_pending_mesh_rebuilds.erase(cx, cy, cz);
    m_mesh_rebuild_requeue.erase(cx, cy, cz);
}

void Chunk_Manager::enqueue_chunk_load(int cx, int cy, int cz) {
    if (is_chunk_loaded(cx, cy, cz) || m_requested_chunks.contains(cx, cy, cz)) {
        return;
    }
    if (!m_world_generator) {
        LOG_ERROR("Chunk_Manager::enqueue_chunk_load: World_Generator is not set");
        return;
    }

    m_requested_chunks.insert(cx, cy, cz);

    // Если чанк уже сохранён (на диске или в очереди записи), его читает пачками m_disk_reader,
    // иначе генерирует воркер.
    const Region_File::Key key{cx, cy, cz};
    const auto region = Region_File::get(m_world_file.get_region_path(m_generation_folder));
    const bool on_disk = region && m_chunk_io->has_saved(region, key);

    // Вертикаль весим сильнее горизонтали: игроку важнее сначала получить свой собственный
    // "этаж" мира по всем сторонам, чем далёкое небо/подземелье в двух шагах по горизонтали.
    // Так пользователь видит законченный, стоящий на земле мир вокруг себя быстрее — даже
    // если весь остальной столбец (все 8 чанков по Y, см. load_around) всё равно рано или
    // поздно догрузится следом.
    const int priority = chunk_job_priority(cx, cy, cz);

    if (on_disk) {
        {
            std::lock_guard<std::mutex> lock(m_job_mutex);
            m_pending_disk_reads.push_back({{cx, cy, cz}, m_generation_id, priority});
        }
        m_job_available.notify_all();
    } else {
        const Chunk_Job job{{cx, cy, cz}, m_generation_id, priority};
        m_pending_jobs.push(job, priority);
        m_pending_job_count.fetch_add(1);
        wake_workers();
    }
}

int Chunk_Manager::chunk_job_priority(int cx, int cy, int cz) const {
    const float dx = static_cast<float>(cx - m_player_chunk.x);
    const float dz = static_cast<float>(cz - m_player_chunk.y);
    const float dy = static_cast<float>(cy - m_player_chunk_y);
    const float dist2 = dx * dx + dz * dz + dy * dy * 4.0f;

    // Смещение к направлению движения: чем быстрее летит игрок (особенно по вертикали),
    // тем сильнее чанки впереди обгоняют чанки сзади. Вектор скорости переводится в
    // "шкалу чанков" (вертикаль весит ×2, как и в расстоянии выше, чтобы направления совпадали).
    const float speed = glm::length(m_player_velocity);
    float factor = 1.0f;
    if (speed > 0.5f && dist2 > 0.0f) {
        const float strength = std::clamp(speed / kSpeedFullBias, 0.0f, 1.0f) * 0.75f;
        const glm::vec3 dir = m_player_velocity / speed;
        const glm::vec3 off = glm::vec3(dx, dy * 2.0f, dz);
        const float cos_angle = glm::dot(off / glm::length(off), dir);
        factor = cos_angle > 0.0f ? 1.0f - strength * cos_angle          // впереди: ближе по приоритету
                                  : 1.0f + strength * 0.5f * -cos_angle; // сзади: дальше по приоритету
    }
    return static_cast<int>(dist2 * factor * 16.0f);
}

size_t Chunk_Manager::max_inflight_chunks() const {
    const float speed = glm::length(m_player_velocity);
    return kBaseInflightChunks + static_cast<size_t>(speed);
}

bool Chunk_Manager::update_vertical_window() {
    const float vy = std::abs(m_player_velocity.y);
    const bool small_radius = m_load_radius <= kSmallRenderRadius;
    const int before = m_vertical_half;
    if (small_radius) {
        m_vertical_half = kVerticalHalfReduced;
    } else if (m_vertical_half == kVerticalHalfNormal) {
        if (vy > kFastVerticalSpeed) m_vertical_half = kVerticalHalfReduced;
    } else if (vy < kSlowVerticalSpeed) {
        m_vertical_half = kVerticalHalfNormal;
    }
    return m_vertical_half != before;
}

uint8_t Chunk_Manager::view_ring_of(const glm::ivec3& position) const {
    const int ring = std::max({std::abs(position.x - m_player_chunk.x),
                               std::abs(position.z - m_player_chunk.y),
                               std::abs(position.y - m_player_chunk_y)});
    return static_cast<uint8_t>(std::min(ring, 255));
}

void Chunk_Manager::refresh_view_rings() {
    ++m_view_generation;
    std::vector<glm::ivec3> entered_near;   // чанки, которые только что вошли в кольца 0–1 без меша
    m_chunks.for_each([&](const glm::ivec3& key, Chunk_Ptr& chunk) {
        const uint8_t before = chunk->get_view_ring();
        const uint8_t ring = view_ring_of(key);
        chunk->set_view_ring(ring, m_view_generation);
        if (ring <= 1 && before > 1 && chunk->get_mesh_version() == 0) {
            entered_near.push_back(key);
        }
    });
    // Ближние чанки без меша не ждут очереди: постановка с приоритетом кольца уходит первой.
    for (const glm::ivec3& key : entered_near) {
        enqueue_mesh_rebuild(key.x, key.y, key.z);
    }
}

void Chunk_Manager::reprioritize_pending_jobs() {
    // Вызывается только с главного потока: забираем все задачи и кладём обратно с новым приоритетом.
    // Задача, которую в этот момент взял воркер, из очереди уже не выйдет повторно.
    std::vector<Chunk_Job> jobs;
    Chunk_Job taken;
    while (m_pending_jobs.try_pop(taken)) {
        m_pending_job_count.fetch_sub(1);
        jobs.push_back(taken);
    }
    for (Chunk_Job& job : jobs) {
        const glm::ivec3& p = job.position;
        if (!is_chunk_wanted(p)) {
            // Игрок ушёл, чанк больше не нужен: задача снимается, а не генерируется впустую.
            m_requested_chunks.erase(p.x, p.y, p.z);
            continue;
        }
        job.priority = chunk_job_priority(p.x, p.y, p.z);
        m_pending_jobs.push(job, job.priority);
        m_pending_job_count.fetch_add(1);
    }
}

void Chunk_Manager::reprioritize_pending_meshes() {
    // Главный поток. Снимаем все задачи мешинга, пересчитываем приоритет по текущему кольцу и
    // расстоянию и возвращаем в очередь. Задачи не снимаются: меш нужен и для чанков позади, просто позже.
    // Задача, которую в этот момент взял воркер, сюда уже не вернётся.
    std::vector<std::unique_ptr<Mesh_Job>> jobs;
    std::unique_ptr<Mesh_Job> taken;
    while (m_pending_mesh_jobs.try_pop(taken)) {
        m_pending_mesh_count.fetch_sub(1);
        jobs.push_back(std::move(taken));
    }
    if (jobs.empty()) return;

    for (std::unique_ptr<Mesh_Job>& job : jobs) {
        const glm::ivec3& p = job->position;
        // Та же формула, что в enqueue_mesh_rebuild: кольцо * 2048 + квадрат расстояния.
        const int dx = std::abs(p.x - m_player_chunk.x);
        const int dz = std::abs(p.z - m_player_chunk.y);
        const int dy = std::abs(p.y - m_player_chunk_y);
        const int ring = static_cast<int>(view_ring_of(p));
        const bool visible = m_visible_chunks.contains(p.x, p.y, p.z);
        // 128 единиц на кольцо дают 64 отдельных кольца в каждом диапазоне очереди (раньше
        // priority bucket queue насыщалась уже примерно на 8-м кольце). Видимые чанки
        // получают первые 8192 единицы приоритета, невидимые — второй диапазон.
        const int priority = (visible ? 0 : 8192) + ring * 128 + std::min(dx * dx + dz * dz + dy * dy * 4, 127);
        job->priority = priority;
        m_pending_mesh_jobs.push(std::move(job), priority);
        m_pending_mesh_count.fetch_add(1);
    }
    wake_workers();
}

void Chunk_Manager::request_urgent_mesh_rebuild(int cx, int cy, int cz) {
    if (!get_chunk(cx, cy, cz)) return;
    if (m_urgent_mesh_set.contains(cx, cy, cz)) return;
    m_urgent_mesh_set.insert(cx, cy, cz);
    m_urgent_mesh_list.push_back({cx, cy, cz});
}

void Chunk_Manager::flush_urgent_mesh_rebuilds() {
    if (m_urgent_mesh_list.empty()) return;
    std::vector<glm::ivec3> list;
    list.swap(m_urgent_mesh_list);
    m_urgent_mesh_set.clear();
    for (const glm::ivec3& p : list) enqueue_mesh_rebuild(p.x, p.y, p.z, true);
}

int Chunk_Manager::first_incomplete_ring() {
    constexpr int kAll = std::numeric_limits<int>::max();
    if (m_player_chunk.x == std::numeric_limits<int>::min()) return kAll;
    // Результат меняется только при загрузке/выгрузке чанков, смене чанка игрока или окна.
    if (m_ring_cache_epoch == m_chunk_set_epoch && m_ring_cache_view == m_view_generation &&
        m_ring_cache_half == m_vertical_half && m_ring_cache_inflight == m_requested_chunks.size()) {
        return m_ring_cache_value;
    }
    m_ring_cache_epoch = m_chunk_set_epoch; m_ring_cache_view = m_view_generation;
    m_ring_cache_half = m_vertical_half; m_ring_cache_inflight = m_requested_chunks.size();
    m_ring_cache_value = kAll;
    const int min_cy = Config::world_min_chunk_y, max_cy = Config::world_max_chunk_y;
    const int max_ring = std::max(m_load_radius, m_vertical_half);
    for (int r = 0; r <= max_ring; ++r) {
        const int vy = std::min(r, m_vertical_half);
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                const bool edge_xz = std::max(std::abs(dx), std::abs(dz)) == r;
                if (std::max(std::abs(dx), std::abs(dz)) > m_load_radius) continue;
                for (int dy = -vy; dy <= vy; ++dy) {
                    if (!edge_xz && std::abs(dy) != r) continue;   // внутри оболочки кольца r
                    const int cy = m_player_chunk_y + dy;
                    if (cy < min_cy || cy > max_cy) continue;
                    if (!is_chunk_loaded(m_player_chunk.x + dx, cy, m_player_chunk.y + dz)) return m_ring_cache_value = r;
                }
            }
        }
    }
    return m_ring_cache_value;
}

void Chunk_Manager::enqueue_mesh_rebuild(int cx, int cy, int cz, bool urgent) {
    const glm::ivec3 position{cx, cy, cz};
    Chunk* target = get_chunk(cx, cy, cz);
    if (!target) {
        return;
    }

    // Чанк целиком из воздуха ничего не рисует (грани на границе рисуют соседи) — задача мешинга
    // с копированием 26 КБ снимка и проходом по воркерам ему не нужна, меш пустой.
    if (!current_capture_clip().active && target->get_blocks().is_uniform() &&
        target->get_blocks().uniform_block() == Block_Types::Air &&
        !m_pending_mesh_rebuilds.contains(cx, cy, cz)) {
        target->apply_mesh(Chunk_Meshes{});
        return;
    }

    if (m_pending_mesh_rebuilds.contains(cx, cy, cz) && !urgent) {
        // Уже летит задача для этого чанка — не дублируем работу, а просто просим
        // догнать ещё раз свежими данными, когда текущая задача вернётся.
        m_mesh_rebuild_requeue.insert(cx, cy, cz);
        return;
    }
    // Срочная правка при уже летящей задаче: ставим вторую задачу со свежим снимком; результат старой
    // отбросится по номеру (seq), так что устаревший меш не затрёт новый.
    m_pending_mesh_rebuilds.insert(cx, cy, cz);

    Mesh_Job job;
    job.position = position;
    job.own_blocks = target->get_blocks();
    job.own_light = target->get_light_grid();
    job.generation_id = m_generation_id;
    // Счётчик пометок снимаем вместе со снимком данных: правка после этого момента его изменит.
    job.dirty_epoch = target->get_dirty_epoch();
    job.instance_id = target->get_instance_id();
    job.clip = current_capture_clip();
    job.urgent = urgent;
    job.seq = target->next_mesh_seq();

    // Порядок ровно как в комментарии к Mesh_Job: -X, +X, -Y, +Y, -Z, +Z.
    static constexpr glm::ivec3 offsets[6] = {
        {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}
    };
    const Capture_Clip clip = job.clip;
    for (int i = 0; i < 6; ++i) {
        const glm::ivec3 npos = position + offsets[i];
        // Область захвата: чанки за её границей могут быть загружены (обычная дальность
        // прорисовки), но для снимка их «нет» — иначе вместо боковой грани среза был бы
        // реальный соседний рельеф.
        if (clip.active && (npos.x < clip.min_cx || npos.x > clip.max_cx ||
                            npos.z < clip.min_cz || npos.z > clip.max_cz)) {
            continue;
        }
        if (const Chunk* neighbor = get_chunk(npos.x, npos.y, npos.z)) {
            Chunk_Block_Face block_face;
            Chunk_Light_Face light_face;
            // Order of faces: -X,+X,-Y,+Y,-Z,+Z.
            if (i == 0 || i == 1) {
                const int x = (i == 0) ? Config::chunk_size - 1 : 0;
                for (int y = 0; y < Config::chunk_height; ++y) {
                    for (int z = 0; z < Config::chunk_size; ++z) {
                        const size_t idx = static_cast<size_t>(y) * Config::chunk_size + z;
                        block_face.data[idx] = static_cast<Block_ID>((neighbor->get_block(x, y, z)));
                        block_face.meta[idx] = neighbor->get_meta(x, y, z);
                        light_face.data[idx] = neighbor->get_packed_light(x, y, z);
                    }
                }
            } else if (i == 2 || i == 3) {
                const int y = (i == 2) ? Config::chunk_height - 1 : 0;
                for (int z = 0; z < Config::chunk_size; ++z) {
                    for (int x = 0; x < Config::chunk_size; ++x) {
                        const size_t idx = static_cast<size_t>(z) * Config::chunk_size + x;
                        block_face.data[idx] = static_cast<Block_ID>((neighbor->get_block(x, y, z)));
                        block_face.meta[idx] = neighbor->get_meta(x, y, z);
                        light_face.data[idx] = neighbor->get_packed_light(x, y, z);
                    }
                }
            } else {
                const int z = (i == 4) ? Config::chunk_size - 1 : 0;
                for (int y = 0; y < Config::chunk_height; ++y) {
                    for (int x = 0; x < Config::chunk_size; ++x) {
                        const size_t idx = static_cast<size_t>(y) * Config::chunk_size + x;
                        block_face.data[idx] = static_cast<Block_ID>((neighbor->get_block(x, y, z)));
                        block_face.meta[idx] = neighbor->get_meta(x, y, z);
                        light_face.data[idx] = neighbor->get_packed_light(x, y, z);
                    }
                }
            }
            job.neighbor_blocks[i] = std::move(block_face);
            job.neighbor_light[i] = std::move(light_face);
        }
    }

    // Рёбра и углы (соседи по диагонали) — для AO. Клетка берётся из загруженного соседа; если его нет,
    // она помечается неизвестной и достаётся генератору в воркере.
    for (int idx = 0; idx < Chunk_Ring::kSize; ++idx) {
        int x = 0, y = 0, z = 0;
        chunk_ring_coords(idx, x, y, z);
        const int ox = x < 0 ? -1 : (x >= Config::chunk_size ? 1 : 0);
        const int oy = y < 0 ? -1 : (y >= Config::chunk_height ? 1 : 0);
        const int oz = z < 0 ? -1 : (z >= Config::chunk_size ? 1 : 0);
        const glm::ivec3 npos = position + glm::ivec3(ox, oy, oz);
        if (clip.active && (npos.x < clip.min_cx || npos.x > clip.max_cx ||
                            npos.z < clip.min_cz || npos.z > clip.max_cz)) {
            job.ring_blocks[idx] = 0;   // за границей захвата — воздух, как и в грани
            job.ring_valid[idx] = true;
            continue;
        }
        if (const Chunk* neighbor = get_chunk(npos.x, npos.y, npos.z)) {
            job.ring_blocks[idx] = static_cast<Block_ID>((neighbor->get_block(
                x - ox * Config::chunk_size, y - oy * Config::chunk_height, z - oz * Config::chunk_size)));
            job.ring_valid[idx] = true;
        }
    }

    // Меньше = срочнее. Правка блока получает самый высокий приоритет, затем близкие
    // чанки, затем обычные фоновые пересборки после генерации соседей.
    // Сначала по кольцу (ближе — срочнее), внутри кольца — по квадрату расстояния.
    const int dx = std::abs(cx - m_player_chunk.x);
    const int dz = std::abs(cz - m_player_chunk.y);
    const int dy = std::abs(cy - m_player_chunk_y);
    const bool visible = m_visible_chunks.contains(cx, cy, cz);
    job.priority = (visible ? 0 : 8192) + static_cast<int>(target->get_view_ring()) * 128 +
                   std::min(dx * dx + dz * dz + dy * dy * 4, 127);
    if (urgent) job.priority = 0;   // правка игрока: самый первый бакет очереди

    const int priority = job.priority;
    m_pending_mesh_jobs.push(std::make_unique<Mesh_Job>(std::move(job)), priority);
    m_pending_mesh_count.fetch_add(1);
    wake_workers();
}

bool Chunk_Manager::is_chunk_wanted(const glm::ivec3& position) const {
    // Во время захвата нужны только чанки плана, обычное окно вокруг игрока не действует.
    if (capture_active()) {
        return capture_plan_contains(position);
    }
    if (m_player_chunk.x == std::numeric_limits<int>::min()) {
        return true;
    }

    // Вертикально столбец загружается целиком (см. Config::world_height_chunks) — здесь
    // Вертикаль теперь тоже стримится: весь диапазон -256..1024 существует, но нет
    // смысла держать все 41 chunk-Y в RAM одновременно.
    const int dx = std::abs(position.x - m_player_chunk.x);
    const int dz = std::abs(position.z - m_player_chunk.y);
    const int dy = std::abs(position.y - m_player_chunk_y);
    return dx <= m_load_radius && dz <= m_load_radius &&
           dy <= m_vertical_half;
}

void Chunk_Manager::set_load_radius(int radius) {
    const int clamped = std::clamp(radius, Config::render_distance_min,
                                   Config::render_distance_max);
    if (clamped == m_load_radius) return;
    m_load_radius = clamped;

    // Сбрасываем запомненную позицию игрока, чтобы update_player_position на
    // следующем кадре гарантированно пересчитал и загрузку, и выгрузку под новый
    // радиус, а не решил, что "игрок не сменил чанк, делать нечего".
    m_player_chunk = {std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
}

void Chunk_Manager::load_around(int player_cx, int player_cz) {
    // Кандидаты — все незагруженные и незапрошенные чанки окна; ставятся в очередь по приоритету
    // и не больше лимита, чтобы очередь не раздувалась на быстром движении. Остальное доливается
    // из update_player_position, когда очередь освободится.
    const size_t cap = max_inflight_chunks();
    const size_t inflight = m_requested_chunks.size();
    if (inflight >= cap) {
        m_load_backlog = true;
        return;
    }

    struct Candidate { glm::ivec3 position; int priority; };
    std::vector<Candidate> candidates;
    const int min_cy = std::max(Config::world_min_chunk_y, m_player_chunk_y - m_vertical_half);
    const int max_cy = std::min(Config::world_max_chunk_y, m_player_chunk_y + m_vertical_half - 1);
    for (int cx = player_cx - m_load_radius; cx <= player_cx + m_load_radius; ++cx) {
        for (int cz = player_cz - m_load_radius; cz <= player_cz + m_load_radius; ++cz) {
            for (int cy = min_cy; cy <= max_cy; ++cy) {
                if (is_chunk_loaded(cx, cy, cz) || m_requested_chunks.contains(cx, cy, cz)) continue;
                candidates.push_back({{cx, cy, cz}, chunk_job_priority(cx, cy, cz)});
            }
        }
    }

    const size_t budget = cap - inflight;
    const size_t take = std::min(budget, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(take),
                      candidates.end(), [](const Candidate& a, const Candidate& b) {
                          return a.priority < b.priority;
                      });
    for (size_t i = 0; i < take; ++i) {
        const glm::ivec3& p = candidates[i].position;
        enqueue_chunk_load(p.x, p.y, p.z);
    }
    m_load_backlog = candidates.size() > take;
}

void Chunk_Manager::unload_distant(int center_cx, int center_cz, int max_distance) {
    // Выгрузка плавная: по высоте держим на один слой больше окна загрузки (сужение окна при быстром
    // полёте ничего не выгружает), а лишнее убираем порциями от самых далёких. Раньше при смене высоты
    // целый слой уходил за один кадр и потом заново генерировался.
    const int keep_half = std::max(m_vertical_half, kVerticalHalfNormal) + 1;
    struct Far { glm::ivec3 key; int ring; };
    std::vector<Far> to_unload;

    m_chunks.for_each([&](const glm::ivec3& key, const Chunk_Ptr&) {
        const int dx = std::abs(key.x - center_cx);
        const int dz = std::abs(key.z - center_cz);
        const int dy = std::abs(key.y - m_player_chunk_y);
        if (dx > max_distance || dz > max_distance || dy > keep_half) {
            to_unload.push_back({key, std::max({dx, dz, dy * 4})});
        }
    });

    constexpr size_t kUnloadPerCall = 24;
    if (to_unload.size() > kUnloadPerCall) {
        std::partial_sort(to_unload.begin(), to_unload.begin() + static_cast<std::ptrdiff_t>(kUnloadPerCall),
                          to_unload.end(), [](const Far& a, const Far& b) { return a.ring > b.ring; });
        to_unload.resize(kUnloadPerCall);
        m_unload_backlog = true;
    } else {
        m_unload_backlog = false;
    }

    for (const Far& far : to_unload) {
        unload_chunk(far.key.x, far.key.y, far.key.z);
    }
}

void Chunk_Manager::update_player_position(float player_x, float player_y, float player_z) {
    // Пока идёт захват области, загрузкой управляет pump_capture_region().
    if (capture_active()) return;

    // Скорость игрока: разность позиций за кадр, сглаженная. Длинная пауза (окно неактивно,
    // телепорт) сбрасывает скорость, чтобы не выстрелить огромным вектором.
    const auto now = std::chrono::steady_clock::now();
    const glm::vec3 pos{player_x, player_y, player_z};
    if (m_has_last_pos) {
        const float dt = std::chrono::duration<float>(now - m_last_pos_time).count();
        if (dt > 0.5f) {
            m_player_velocity = glm::vec3(0.0f);
        } else if (dt > 1e-4f) {
            const glm::vec3 raw = (pos - m_last_player_pos) / dt;
            m_player_velocity = glm::mix(m_player_velocity, raw, 0.3f);
        }
    }
    m_last_player_pos = pos;
    m_last_pos_time = now;
    m_has_last_pos = true;

    flush_urgent_mesh_rebuilds();
    collect_finished_chunks();
    collect_finished_mesh_jobs();

    const int player_cx = static_cast<int>(std::floor(player_x / Config::chunk_size));
    const int player_cz = static_cast<int>(std::floor(player_z / Config::chunk_size));
    const int player_cy = static_cast<int>(std::floor(player_y / Config::chunk_height));

    const bool horizontal_changed = player_cx != m_player_chunk.x || player_cz != m_player_chunk.y;
    const bool vertical_changed = player_cy != m_player_chunk_y;
    const bool window_changed = update_vertical_window();
    if (!horizontal_changed && !vertical_changed && !window_changed) {
        if (m_unload_backlog) unload_distant(m_player_chunk.x, m_player_chunk.y, m_load_radius);
        // Очередь могла освободиться: доливаем то, что не влезло при прошлом запросе.
        if (m_load_backlog && m_requested_chunks.size() < max_inflight_chunks() / 2) {
            load_around(m_player_chunk.x, m_player_chunk.y);
        }
        rebuild_dirty_chunks();
        return;
    }

    m_player_chunk = {player_cx, player_cz};
    m_player_chunk_y = player_cy;
    refresh_view_rings();

    // Очередь пересортировывается по новой позиции до постановки новых задач: ближайшие к игроку
    // чанки всегда идут первыми, а дальние задачи, оставшиеся позади, снимаются.
    reprioritize_pending_jobs();
    reprioritize_pending_meshes();
    unload_distant(player_cx, player_cz, m_load_radius);
    load_around(player_cx, player_cz);
    rebuild_dirty_chunks();
}

void Chunk_Manager::collect_finished_chunks() {

    bool any_chunk_inserted = false;

    // Вставок в кадр тем больше, чем быстрее летит игрок: при 4 за кадр скорость подгрузки
    // ограничена сверху (~240 чанков/с), и на большой скорости дальние слои не успевают.
    const float speed = glm::length(m_player_velocity);
    const size_t chunk_cap = capture_active()
        ? capture_max_finished_chunks_per_frame
        : std::max(max_finished_chunks_per_frame, static_cast<size_t>(4.0f + speed / 8.0f));
    size_t processed_chunks = 0;
    while (processed_chunks < chunk_cap) {
        Chunk_Result result;
        if (!m_completed_jobs.try_pop(result)) break;
        ++processed_chunks;

        m_requested_chunks.erase(result.position.x, result.position.y, result.position.z);

        if (!result.chunk || result.generation_id != m_generation_id ||
            is_chunk_loaded(result.position.x, result.position.y, result.position.z) ||
            !is_chunk_wanted(result.position)) {
            continue;
        }

        result.chunk->set_chunk_manager(this);
        result.chunk->set_texture_atlas(m_texture_atlas);
        // insert_or_assign возвращает указатель на только что вставленный чанк напрямую —
        // не нужен повторный get_chunk() (отдельный поиск в таблице) сразу после вставки,
        // как было раньше.
        Chunk* new_chunk = m_chunks.insert_or_assign(result.position.x, result.position.y,
                                                       result.position.z, std::move(result.chunk));
        any_chunk_inserted = true;
        new_chunk->set_view_ring(view_ring_of(result.position), m_view_generation);
        if (new_chunk->get_view_ring() <= 1 && new_chunk->get_mesh_version() == 0) {
            enqueue_mesh_rebuild(result.position.x, result.position.y, result.position.z);
        }
        ++m_chunk_set_epoch;
        m_columns_pending_sky_recompute.insert(
            (static_cast<uint64_t>(static_cast<uint32_t>(result.position.x)) << 32) |
            static_cast<uint32_t>(result.position.z));

        // Источники света, которые пришли с чанком (в том числе из сохранённого мира): без этого
        // факелы и солнечные камни с диска не попадали в m_light_sources и не участвовали в пересчёте.
        // Та же проверка заодно ставит тики жидкостям: вода, пришедшая с чанком (например, река
        // из генератора), соприкасается с воздухом и должна начать растекаться. Без тика такая вода
        // стояла бы "блоками", пока её не тронут.
        const uint8_t* emission = Block_Registry::get_instance().light_emission_table();
        const Block_Types first_block = new_chunk->get_block(0, 0, 0);
        const bool uniform_idle = new_chunk->get_blocks().is_uniform() &&
            emission[static_cast<Block_ID>(first_block)] == 0 &&
            get_block_props(first_block).state != Block_State::Liquid;
        if (!uniform_idle) {
            const int base_x = result.position.x * Config::chunk_size;
            const int base_y = result.position.y * Config::chunk_height;
            const int base_z = result.position.z * Config::chunk_size;
            static constexpr glm::ivec3 k_open_side[5] = {
                {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}
            };
            for (int ly = 0; ly < Config::chunk_height; ++ly) {
                for (int lz = 0; lz < Config::chunk_size; ++lz) {
                    for (int lx = 0; lx < Config::chunk_size; ++lx) {
                        const Block_Types block = new_chunk->get_block(lx, ly, lz);
                        const auto id = static_cast<Block_ID>(block);
                        const glm::ivec3 pos{base_x + lx, base_y + ly, base_z + lz};
                        if (emission[id] > 0 &&
                            std::find(m_light_sources.begin(), m_light_sources.end(), pos) == m_light_sources.end()) {
                            m_light_sources.push_back(pos);
                        }
                        const Block_Properties& props = get_block_props(block);
                        if (props.state != Block_State::Liquid) continue;
                        for (const glm::ivec3& d : k_open_side) {
                            const Block_Types nb = get_block_world(pos.x + d.x, pos.y + d.y, pos.z + d.z);
                            const Block_Properties& np = get_block_props(nb);
                            if (nb == Block_Types::Air || (!np.is_solid && np.state != Block_State::Liquid)) {
                                m_fluids.schedule(pos, props.fluid_tick_delay);
                                break;
                            }
                        }
                    }
                }
            }
        }

        // Чанк с текущей водой (метаданные жидкости хранятся только у течений): тики
        // не сохраняются, поэтому назначаем их заново — иначе течение, оставшееся без источника
        // до выгрузки, так и висело бы в воздухе.
        if (new_chunk->get_blocks().has_meta()) {
            const int base_x = result.position.x * Config::chunk_size;
            const int base_y = result.position.y * Config::chunk_height;
            const int base_z = result.position.z * Config::chunk_size;
            for (const auto& [index, value] : new_chunk->get_blocks().meta_entries()) {
                (void)value;
                const int x = index % Config::chunk_size;
                const int z = (index / Config::chunk_size) % Config::chunk_size;
                const int y = index / (Config::chunk_size * Config::chunk_size);
                const Block_Types block = new_chunk->get_block(x, y, z);
                if (get_block_props(block).state == Block_State::Liquid) {
                    m_fluids.schedule({base_x + x, base_y + y, base_z + z},
                                      get_block_props(block).fluid_tick_delay);
                }
            }
        }

        // Mesh generation is intentionally deferred until insertion: only then can
        // the async job snapshot include currently loaded neighbors.
        new_chunk->mark_mesh_dirty();

        // Все 26 соседей: AO читает клетки по рёбрам и углам, а также вертикальные швы по Y
        // (потолок пещеры на границе секции), поэтому помечаем весь куб окрестности.
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((dx | dy | dz) == 0) continue;
                    if (Chunk* neighbor = get_chunk(result.position.x + dx, result.position.y + dy,
                                                    result.position.z + dz)) {
                        neighbor->mark_mesh_dirty();
                    }
                }
            }
        }
    }

    // Новые чанки родились с пустой (нулевой) сеткой света — если рядом уже есть источники,
    // их свет должен "дотянуться" и в свежезагруженные соседние чанки. Пересчёт полный, но
    // дешёвый (см. recompute_lighting) и запускается только когда реально что-то догрузилось
    // И есть хоть один источник — иначе на каждый обычный кадр это было бы лишней работой.
    // Do not rebuild the entire skylight field after every single generated chunk. During
    // startup the vertical stack can contain thousands of chunks; rebuilding after each
    // insertion turns loading into a quadratic amount of work and makes the window appear
    // frozen. Wait until the current load wave has drained, then build it once.
    // Захват области делает свет отдельной фазой (pump_capture_region): после загрузки ВСЕХ
    // чанков, один раз, а не на каждой волне.
    if (capture_active()) return;

    // Свет неба считается ДО первого меша колонки: меш запекает свет, и чанк, собранный по нулевой
    // сетке, потом приходится пересобирать (раньше это было 8 колонок раз в 30 кадров, и чанки
    // дольше всего оставались тёмными). Колонка берётся, когда в ней не осталось запрошенных,
    // но ещё не загруженных чанков; ближайшие к игроку — первыми, в пределах бюджета времени.
    if (m_columns_pending_sky_recompute.empty()) return;

    const bool have_player = m_player_chunk.x != std::numeric_limits<int>::min();
    const int min_cy = have_player ? std::max(Config::world_min_chunk_y, m_player_chunk_y - m_vertical_half)
                                   : Config::world_min_chunk_y;
    const int max_cy = have_player ? std::min(Config::world_max_chunk_y, m_player_chunk_y + m_vertical_half)
                                   : Config::world_max_chunk_y;
    struct Column { uint64_t key; int score; };
    std::vector<Column> ready;
    for (const uint64_t key : m_columns_pending_sky_recompute) {
        const int cx = static_cast<int>(static_cast<int32_t>(key >> 32));
        const int cz = static_cast<int>(static_cast<int32_t>(key & 0xFFFFFFFFu));
        bool settled = true;
        if (!m_requested_chunks.empty()) {
            for (int cy = min_cy; cy <= max_cy && settled; ++cy) {
                if (m_requested_chunks.contains(cx, cy, cz)) settled = false;
            }
        }
        if (!settled) continue;
        int score = 0;
        if (have_player) {
            const int dx = cx - m_player_chunk.x;
            const int dz = cz - m_player_chunk.y;
            score = dx * dx + dz * dz;
        }
        ready.push_back({key, score});
    }
    if (ready.empty()) return;
    std::sort(ready.begin(), ready.end(), [](const Column& a, const Column& b) { return a.score < b.score; });

    // Только вертикальный свет, пустые чанки пропускаются; горизонтальный flood-fill отложен до
    // ручных изменений блоков. Бюджет по времени вместо фиксированного числа колонок.
    constexpr double sky_budget_ms = 1.5;
    constexpr size_t sky_columns_per_step = 2;
    const auto sky_start = std::chrono::steady_clock::now();
    bool any = false;
    for (size_t i = 0; i < ready.size();) {
        std::unordered_set<uint64_t> batch;
        for (size_t n = 0; n < sky_columns_per_step && i < ready.size(); ++n, ++i) {
            batch.insert(ready[i].key);
            m_columns_pending_sky_recompute.erase(ready[i].key);
        }
        recompute_sky_lighting(batch, false);
        any = true;
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sky_start).count();
        if (elapsed_ms >= sky_budget_ms) break;
    }
    m_frames_since_sky_recompute = 0;
    if (any && !m_light_sources.empty()) {
        recompute_lighting();
    }
}

void Chunk_Manager::collect_finished_mesh_jobs() {

    const size_t mesh_cap = capture_active() ? capture_max_finished_meshes_per_frame
                                             : max_finished_meshes_per_frame;
    size_t processed_meshes = 0;
    // Минимум mesh_cap результатов за вызов, дальше — пока не вышел бюджет времени (потолок 8x):
    // фиксированные 4 штуки не успевали за воркерами, а дорогие кадры от этого не появляются.
    constexpr double apply_budget_ms = 1.0;
    const auto apply_start = std::chrono::steady_clock::now();
    const size_t mesh_hard_cap = mesh_cap * 8;
    while (true) {
        Mesh_Result result;
        // Срочные результаты (правка блока) применяются сразу, без лимита кадра.
        const bool is_urgent = m_completed_urgent_mesh_jobs.try_pop(result);
        if (!is_urgent) {
            if (processed_meshes >= mesh_hard_cap) break;
            if (processed_meshes >= mesh_cap &&
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - apply_start).count()
                    >= apply_budget_ms) break;
            if (!m_completed_mesh_jobs.try_pop(result)) break;
            ++processed_meshes;
        }

        // Мир пересоздан (F6/новый seed) уже после того, как эта задача была поставлена —
        // результат относится к чанкам, которых больше нет. Тихо отбрасываем. Состояние позиции
        // (pending/requeue) при этом не трогаем: оно уже может принадлежать новой задаче.
        if (result.generation_id != m_generation_id) {
            continue;
        }

        // Чанк успели выгрузить (игрок ушёл далеко), пока задача летала в воркере — применять
        // результат некуда. Хуже того: на той же позиции мог уже появиться ДРУГОЙ объект (выгрузили
        // и загрузили заново). Тогда результат устарел, а pending/requeue позиции относятся к новой
        // задаче. Поэтому сверяем объект и только при совпадении трогаем состояние позиции.
        Chunk* chunk = get_chunk(result.position.x, result.position.y, result.position.z);
        if (!chunk || chunk->get_instance_id() != result.instance_id) {
            continue;
        }

        // Позже поставлена более новая задача (срочная правка): этот результат устарел. Состояние позиции
        // принадлежит новой задаче и остаётся как есть.
        if (result.seq < chunk->get_mesh_seq_issued()) {
            continue;
        }

        m_pending_mesh_rebuilds.erase(result.position.x, result.position.y, result.position.z);
        const bool should_requeue =
            m_mesh_rebuild_requeue.erase(result.position.x, result.position.y, result.position.z);

        // Пометка грязности пришла после снимка этой задачи (например, свет соседа пересчитали уже
        // после постановки): снимок её не учитывает. apply_mesh снимает флаг, поэтому без сравнения
        // счётчика такая правка терялась бы, и меш оставался устаревшим.
        const bool dirtied_after_snapshot = chunk->get_dirty_epoch() != result.dirty_epoch;
        chunk->apply_mesh(std::move(result.meshes));

        // Блок поменялся ещё раз, пока эта пересборка считалась в фоне — снимок, с которым
        // она стартовала, уже устарел. Ставим ещё одну задачу со свежими данными, чтобы
        // не потерять последнюю правку молча.
        if (should_requeue || dirtied_after_snapshot) {
            enqueue_mesh_rebuild(result.position.x, result.position.y, result.position.z);
        }
    }
}

void Chunk_Manager::rebuild_dirty_chunks() {
    const float speed = glm::length(m_player_velocity);
    const size_t enqueue_cap = capture_active()
        ? capture_max_mesh_jobs_enqueued_per_frame
        : std::max(max_mesh_jobs_enqueued_per_frame, static_cast<size_t>(16.0f + speed / 4.0f));

    // Раньше обход шёл в порядке хеш-таблицы: порядок мешинга выходил случайным, лимит тратился на
    // чанки, у которых задача уже летела, а каждый такой вызов ещё и взводил requeue (лишняя
    // пересборка). Теперь собираем только реальных кандидатов (грязные, без летящей задачи, с
    // готовым окружением) и отдаём в работу ближайших к игроку; в лимит идут только настоящие постановки.
    const bool have_player = m_player_chunk.x != std::numeric_limits<int>::min();
    const bool stream_idle = m_requested_chunks.empty() && !m_load_backlog;
    const int min_cy = Config::world_min_chunk_y;
    const int max_cy = Config::world_max_chunk_y;
    const auto neighbors_settled = [&](const glm::ivec3& p) {
        if (capture_active() || stream_idle) return true;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((dx | dy | dz) == 0) continue;
                    const glm::ivec3 n = p + glm::ivec3(dx, dy, dz);
                    if (n.y < min_cy || n.y > max_cy) continue;
                    if (is_chunk_loaded(n.x, n.y, n.z)) continue;
                    if (m_requested_chunks.contains(n.x, n.y, n.z)) return false;
                    // Не загружен и не запрошен: либо вне окна (не будет никогда), либо ещё не запрошен.
                    if (is_chunk_wanted(n)) return false;
                }
            }
        }
        return true;
    };

    // Меши появляются строго от центра к краю: чанк кольца r строится, только когда все нужные чанки колец
    // до r включительно загружены (иначе порядок задавало бы время генерации, и картина шла бы пятнами).
    const int ring_limit = (capture_active() || stream_idle) ? std::numeric_limits<int>::max()
                                                             : first_incomplete_ring();

    struct Candidate { glm::ivec3 pos; int score; };
    std::vector<Candidate> candidates;
    m_chunks.for_each([&](const glm::ivec3& key, const Chunk_Ptr& chunk) {
        if (!chunk->is_mesh_dirty()) return;
        if (ring_limit != std::numeric_limits<int>::max() && chunk->get_mesh_version() == 0 &&
            static_cast<int>(view_ring_of(key)) + 1 >= ring_limit) return;
        if (m_pending_mesh_rebuilds.contains(key.x, key.y, key.z)) return;
        if (!capture_active() && !m_columns_pending_sky_recompute.empty() &&
            m_columns_pending_sky_recompute.count(
                (static_cast<uint64_t>(static_cast<uint32_t>(key.x)) << 32) |
                static_cast<uint32_t>(key.z))) return;   // свет неба колонки ещё не посчитан
        if (!neighbors_settled(key)) return;
        int score = 0;
        if (have_player) {
            const int dx = key.x - m_player_chunk.x;
            const int dz = key.z - m_player_chunk.y;
            const int dy = key.y - m_player_chunk_y;
            score = dx * dx + dz * dz + dy * dy * 4;
        }
        candidates.push_back({key, score});
    });
    if (candidates.empty()) return;

    const size_t take = std::min(enqueue_cap, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + take, candidates.end(),
                      [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
    for (size_t i = 0; i < take; ++i) {
        enqueue_mesh_rebuild(candidates[i].pos.x, candidates[i].pos.y, candidates[i].pos.z);
    }
}

void Chunk_Manager::clear() {
    ++m_generation_id;
    m_chunks.for_each([this](const glm::ivec3& position, const Chunk_Ptr& chunk) {
        save_chunk_to_disk(*chunk);
        m_unloaded_since_last_query.push_back(position);
    });
    // Индекс region-файла живёт в памяти между записями — здесь он уезжает на диск.
    m_world_file.flush_chunks(m_generation_folder);
    m_chunks.clear();
    ++m_chunk_set_epoch;
    m_requested_chunks.clear();
    m_pending_mesh_rebuilds.clear();
    m_mesh_rebuild_requeue.clear();
    m_light_sources.clear();
    m_fluids.clear();
    m_columns_pending_sky_recompute.clear();
    m_frames_since_sky_recompute = 0;
    m_player_chunk = {std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
    m_player_chunk_y = 0;

    {
        std::lock_guard<std::mutex> lock(m_job_mutex);
        m_pending_jobs.clear();
        m_pending_job_count.store(0);
        m_pending_mesh_jobs.clear();
        m_pending_mesh_count.store(0);
    }
    m_completed_jobs.clear();
    m_completed_mesh_jobs.clear();
    m_completed_urgent_mesh_jobs.clear();
    m_urgent_mesh_set.clear();
    m_urgent_mesh_list.clear();
}

Chunk* Chunk_Manager::get_chunk(int cx, int cy, int cz) {
    return m_chunks.find(cx, cy, cz);
}

const Chunk* Chunk_Manager::get_chunk(int cx, int cy, int cz) const {
    return m_chunks.find(cx, cy, cz);
}

Block_Types Chunk_Manager::get_block_world(int wx, int wy, int wz) const {
    if (wy < Config::world_min_y || wy > Config::world_max_y) {
        return Block_Types::Air;
    }

    const int cx = Math_Helpers::floor_div(wx, Config::chunk_size);
    const int cy = Math_Helpers::floor_div(wy, Config::chunk_height);
    const int cz = Math_Helpers::floor_div(wz, Config::chunk_size);
    const int lx = wx - cx * Config::chunk_size;
    const int ly = wy - cy * Config::chunk_height;
    const int lz = wz - cz * Config::chunk_size;

    const Chunk* chunk = get_chunk(cx, cy, cz);
    if (chunk) {
        return chunk->get_block(lx, ly, lz);
    }

    if (m_world_generator) {
        return m_world_generator->get_block(wx, wy, wz);
    }

    return Block_Types::Air;
}

uint8_t Chunk_Manager::get_block_meta_world(int wx, int wy, int wz) const {
    if (wy < Config::world_min_y || wy > Config::world_max_y) return 0;
    const int cx = Math_Helpers::floor_div(wx, Config::chunk_size);
    const int cy = Math_Helpers::floor_div(wy, Config::chunk_height);
    const int cz = Math_Helpers::floor_div(wz, Config::chunk_size);
    const Chunk* chunk = get_chunk(cx, cy, cz);
    if (!chunk) return 0;
    return chunk->get_meta(wx - cx * Config::chunk_size, wy - cy * Config::chunk_height,
                           wz - cz * Config::chunk_size);
}

bool Chunk_Manager::set_block_meta_world(int wx, int wy, int wz, uint8_t meta) {
    if (wy < Config::world_min_y || wy > Config::world_max_y) return false;
    const int cx = Math_Helpers::floor_div(wx, Config::chunk_size);
    const int cy = Math_Helpers::floor_div(wy, Config::chunk_height);
    const int cz = Math_Helpers::floor_div(wz, Config::chunk_size);
    Chunk* chunk = get_chunk(cx, cy, cz);
    if (!chunk) return false;
    const int lx = wx - cx * Config::chunk_size;
    const int ly = wy - cy * Config::chunk_height;
    const int lz = wz - cz * Config::chunk_size;
    if (chunk->get_meta(lx, ly, lz) == meta) return false;
    chunk->set_meta(lx, ly, lz, meta);
    // Метаданные влияют только на грани самого блока (соседние чанки не пересобираем).
    request_urgent_mesh_rebuild(cx, cy, cz);
    return true;
}

bool Chunk_Manager::set_block_world(int wx, int wy, int wz, Block_Types type, uint8_t meta) {
    if (wy < Config::world_min_y || wy > Config::world_max_y) {
        return false;
    }

    const int cx = Math_Helpers::floor_div(wx, Config::chunk_size);
    const int cy = Math_Helpers::floor_div(wy, Config::chunk_height);
    const int cz = Math_Helpers::floor_div(wz, Config::chunk_size);
    const int lx = wx - cx * Config::chunk_size;
    const int ly = wy - cy * Config::chunk_height;
    const int lz = wz - cz * Config::chunk_size;

    Chunk* chunk = get_chunk(cx, cy, cz);
    if (!chunk) {
        return false;
    }

    const Block_Types old_type = chunk->get_block(lx, ly, lz);
    chunk->set_block(lx, ly, lz, type);
    if (meta != 0) chunk->set_meta(lx, ly, lz, meta);
    sync_block_entity(wx, wy, wz, old_type, type);
    request_urgent_mesh_rebuild(cx, cy, cz);

    // Правленая клетка видна мешам тех соседей, в чьей 3x3x3 окрестности она лежит: для граней —
    // шесть соседей, для AO — ещё рёбра и углы. Пересобираем каждого загруженного из них.
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dx | dy | dz) == 0) continue;
                if ((dx < 0 && lx != 0) || (dx > 0 && lx != Config::chunk_size - 1)) continue;
                if ((dy < 0 && ly != 0) || (dy > 0 && ly != Config::chunk_height - 1)) continue;
                if ((dz < 0 && lz != 0) || (dz > 0 && lz != Config::chunk_size - 1)) continue;
                if (get_chunk(cx + dx, cy + dy, cz + dz)) {
                    request_urgent_mesh_rebuild(cx + dx, cy + dy, cz + dz);
                }
            }
        }
    }

    // Отслеживаем источники света отдельным списком позиций (см. m_light_sources) — только
    // блоки с ненулевым light_emission (сейчас это только Sun Stone, но список не завязан
    // на конкретное имя блока). Пересчёт освещения запускаем в двух случаях: (а) сам этот
    // блок стал или перестал быть источником, (б) правка произошла в радиусе действия УЖЕ
    // существующего источника — например, поставили/убрали стену рядом с Sun Stone. Без (б)
    // такая стена не отбрасывала бы тень, пока что-то ещё не потревожит освещение отдельно —
    // поймано ровно на этом изолированным тестом (свет "просвечивал" сквозь новую стену).
    const bool old_emits_light = get_block_props(old_type).light_emission > 0;
    const bool new_emits_light = get_block_props(type).light_emission > 0;
    const glm::ivec3 world_pos{wx, wy, wz};

    // Раньше пересчёт блочного света запускался только если сам блок стал/перестал быть
    // источником, либо правка попадала в радиус УЖЕ существующего источника. На практике это
    // означало, что перестройка света была заметна только при ломании блоков рядом с уже
    // работающим источником — например, только что поставленный источник света прорисовывался
    // с задержкой/не сразу, если рядом ещё не было других источников. Теперь пересчитываем
    // (дёшево — recompute_lighting сам ограничен окрестностью m_light_sources, а не всем миром)
    // при КАЖДОЙ правке блока, так что новый источник загорается сразу же, как только его
    // поставили, а не только когда что-то другое потревожит освещение по соседству.
    std::optional<glm::ivec3> removed_source_position;
    if (old_emits_light) {
        m_light_sources.erase(
            std::remove(m_light_sources.begin(), m_light_sources.end(), world_pos),
            m_light_sources.end()
        );
        removed_source_position = world_pos;
    }
    if (new_emits_light) {
        m_light_sources.push_back(world_pos);
    }
    // Если блок и до, и после прозрачен для света и ничего не излучает (воздух <-> вода, цветок
    // <-> воздух, смена уровня жидкости), освещение измениться не может: пересчёт пропускаем.
    // Иначе каждый шаг растекания воды гонял бы пересчёт света по целому столбцу чанков.
    const bool* transparency = Block_Registry::get_instance().transparent_table();
    const bool light_unaffected =
        transparency[static_cast<Block_ID>(old_type) & 0xFFu] &&
        transparency[static_cast<Block_ID>(type) & 0xFFu] &&
        !old_emits_light && !new_emits_light;
    if (!light_unaffected) {
        recompute_lighting(removed_source_position);

        // Изменение непрозрачности может открыть/закрыть вертикальный луч солнца или вход в пещеру.
        // Only this one column needs rescanning - the BFS below still spreads sideways into
        // neighboring columns/chunks as needed, same as it always has.
        recompute_sky_lighting({(static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
                                 static_cast<uint32_t>(cz)});
    }

    // Раньше здесь на КАЖДЫЙ поставленный блок целиком переписывался 32-килобайтный
    // чанк — синхронно, из главного потока, прямо в кадре. Теперь правка только
    // помечает чанк изменённым: на диск он уедет при выгрузке, при выходе и при
    // явном сохранении мира (Chunk_Manager::save_all).
    chunk->mark_modified();

    // Соседние жидкости должны отреагировать на правку (вода хлынет в пробитую стену).
    m_fluids.notify_changed(*this, world_pos);

    return true;
}

glm::ivec3 Chunk_Manager::get_chunk_coords_for_world(int wx, int wy, int wz) const {
    return {
        Math_Helpers::floor_div(wx, Config::chunk_size),
        Math_Helpers::floor_div(wy, Config::chunk_height),
        Math_Helpers::floor_div(wz, Config::chunk_size)
    };
}

uint8_t Chunk_Manager::get_light_world(int wx, int wy, int wz) const {
    if (wy < Config::world_min_y || wy > Config::world_max_y) {
        return 0;
    }
    const glm::ivec3 c = get_chunk_coords_for_world(wx, wy, wz);
    const Chunk* chunk = get_chunk(c.x, c.y, c.z);
    if (!chunk) {
        return 0;
    }
    const int lx = wx - c.x * Config::chunk_size;
    const int ly = wy - c.y * Config::chunk_height;
    const int lz = wz - c.z * Config::chunk_size;
    return chunk->get_light(lx, ly, lz);
}

uint8_t Chunk_Manager::get_packed_light_world(int wx, int wy, int wz) const {
    if (wy < Config::world_min_y || wy > Config::world_max_y) return 0;
    const glm::ivec3 c = get_chunk_coords_for_world(wx, wy, wz);
    const Chunk* chunk = get_chunk(c.x, c.y, c.z);
    if (!chunk) return 0;
    const int lx = wx - c.x * Config::chunk_size;
    const int ly = wy - c.y * Config::chunk_height;
    const int lz = wz - c.z * Config::chunk_size;
    return chunk->get_packed_light(lx, ly, lz);
}

bool Chunk_Manager::set_light_world(int wx, int wy, int wz, uint8_t level) {
    if (wy < Config::world_min_y || wy > Config::world_max_y) {
        return false;
    }
    const glm::ivec3 c = get_chunk_coords_for_world(wx, wy, wz);
    Chunk* chunk = get_chunk(c.x, c.y, c.z);
    if (!chunk) {
        return false;
    }
    const int lx = wx - c.x * Config::chunk_size;
    const int ly = wy - c.y * Config::chunk_height;
    const int lz = wz - c.z * Config::chunk_size;
    chunk->set_light(lx, ly, lz, level);
    return true;
}

void Chunk_Manager::recompute_sky_lighting(
    const std::unordered_set<uint64_t>& changed_chunk_columns,
    bool propagate_horizontal
) {
    // Rebuild skylight only for the (chunk_x, chunk_z) columns that gained a newly loaded
    // chunk since the last call - NOT the whole currently loaded voxel volume.
    // IMPORTANT: an earlier version of this function rebuilt the entire loaded world every
    // time it ran, on the theory that it would only ever run once per load wave (see the
    // call site). Once it also had to run periodically as a fallback - so lighting can't
    // stay stuck dark while the request queue never quite empties, e.g. while the player
    // keeps walking - a full-world redo running several times a second is what made the
    // game hang and get killed by the OS. Scoping the clear + rescan to just the columns
    // that changed keeps the cost proportional to how much NEW terrain streamed in since the
    // last call, not to the size of the already-loaded (and already-correctly-lit) world.
    if (changed_chunk_columns.empty()) {
        return;
    }

    // Чанки, у которых свет неба обнулили (их соседи тоже должны перестроить грани на стыке).
    std::unordered_set<glm::ivec3, Chunk_Key_Hash> cleared_chunks;

    for (const uint64_t packed_cc : changed_chunk_columns) {
        const int cx = static_cast<int>(static_cast<int32_t>(packed_cc >> 32));
        const int cz = static_cast<int>(static_cast<int32_t>(packed_cc & 0xFFFFFFFFu));
        for (int cy = Config::world_max_chunk_y; cy >= Config::world_min_chunk_y; --cy) {
            if (Chunk* chunk = get_chunk(cx, cy, cz)) {
                // Пустые вертикальные чанки не содержат блоков и не могут
                // остановить луч солнца. Не трогаем их voxel-by-voxel.
                if (!chunk->has_blocks()) {
                    continue;
                }
                chunk->clear_sky_light();
                cleared_chunks.insert({cx, cy, cz});
                // Очистка света тоже меняет данные, даже если последующий
                // вертикальный проход не установит ни одного нового значения.
                // Иначе старый меш продолжает содержать прежнюю яркость.
                chunk->mark_mesh_dirty();
            }
        }
    }

    std::unordered_set<glm::ivec3, Chunk_Key_Hash> touched_chunks;

    // Плоская таблица прозрачности (см. подробный комментарий у Block_Registry::
    // transparent_table()) — берём ОДИН раз на всю функцию: и для вертикального прохода
    // ниже, и для горизонтальной детекции границы/BFS дальше. Раньше каждый из этих
    // вокселей отдельно дёргал get_block_props(block).is_transparent — тяжёлая структура
    // Block_Properties ради одного bool плюс проверка guard-переменной singleton'а на
    // каждый вызов.
    const bool* transparent = Block_Registry::get_instance().transparent_table();

    // Walk every block column belonging to one of the changed chunk-columns, top to bottom -
    // same per-column logic as before, just restricted to these columns instead of every
    // column in the whole loaded world.
    for (const uint64_t packed_cc : changed_chunk_columns) {
        const int cx = static_cast<int>(static_cast<int32_t>(packed_cc >> 32));
        const int cz = static_cast<int>(static_cast<int32_t>(packed_cc & 0xFFFFFFFFu));

        for (int lx = 0; lx < Config::chunk_size; ++lx) {
            for (int lz = 0; lz < Config::chunk_size; ++lz) {
                // Прямой свет неба идёт сверху вниз. Ближайший непрозрачный блок сверху
                // (occluder_y) отбрасывает тень только если находится не дальше
                // Config::sky_shadow_range; дальше тени нет, и клетка остаётся освещённой
                // (например, земля под парящим островом). Скан не обрывается на первой
                // крыше: ниже может снова встретиться открытое небо.
                constexpr int no_occluder = std::numeric_limits<int>::max();
                int occluder_y = no_occluder;

                for (int cy = Config::world_max_chunk_y; cy >= Config::world_min_chunk_y; --cy) {
                    Chunk* chunk = get_chunk(cx, cy, cz);
                    if (!chunk) continue;
                    if (!chunk->has_blocks()) continue;

                    for (int ly = Config::chunk_height - 1; ly >= 0; --ly) {
                        const int wy = cy * Config::chunk_height + ly;
                        if (wy < Config::world_min_y || wy > Config::world_max_y) continue;

                        const Block_ID block_id = static_cast<Block_ID>(chunk->get_block(lx, ly, lz));
                        if (!transparent[block_id]) {
                            occluder_y = wy;
                            continue;
                        }

                        const bool shadowed = occluder_y != no_occluder &&
                                              occluder_y - wy <= Config::sky_shadow_range;
                        const uint8_t sky = shadowed ? 0 : 15;

                        if (chunk->get_sky_light(lx, ly, lz) != sky) {
                            chunk->set_sky_light(lx, ly, lz, sky);
                            touched_chunks.insert({cx, cy, cz});
                        }
                    }
                }
            }
        }
    }

    if (!propagate_horizontal) {
        for (const auto& pos : touched_chunks) {
            if (Chunk* chunk = get_chunk(pos.x, pos.y, pos.z)) {
                chunk->mark_mesh_dirty();
                chunk->compact_light();
            }
        }
        mark_light_neighbors_dirty(cleared_chunks);
        mark_light_neighbors_dirty(touched_chunks);
        return;
    }

    struct Sky_BFS_Node { Voxel_Cursor at; uint8_t level; };
    Flat_Queue<Sky_BFS_Node> flat_queue;

    // В открытом небе почти каждая прозрачная клетка получает уровень 15.
    // Раньше каждая такая клетка сразу попадала в BFS, хотя соседние клетки
    // с тем же уровнем всё равно ничего не меняли. Запускаем BFS только от
    // границы уже освещенной вертикальной области — там, где свет действительно
    // может уйти в соседнюю темную колонку или пещеру.
    for (const uint64_t packed_cc : changed_chunk_columns) {
        const int cx = static_cast<int>(static_cast<int32_t>(packed_cc >> 32));
        const int cz = static_cast<int>(static_cast<int32_t>(packed_cc & 0xFFFFFFFFu));

        for (int lx = 0; lx < Config::chunk_size; ++lx) {
            for (int lz = 0; lz < Config::chunk_size; ++lz) {
                for (int cy = Config::world_max_chunk_y; cy >= Config::world_min_chunk_y; --cy) {
                    Chunk* chunk = get_chunk(cx, cy, cz);
                    if (!chunk) continue;

                    for (int ly = 0; ly < Config::chunk_height; ++ly) {
                        const int wy = cy * Config::chunk_height + ly;
                        if (wy < Config::world_min_y || wy > Config::world_max_y) continue;

                        const uint8_t level = chunk->get_sky_light(lx, ly, lz);
                        if (level <= 1) continue;

                        const Voxel_Cursor here{cx, cy, cz, lx, ly, lz, chunk};
                        // Раньше для ВСЕХ 4 соседей по x/z всегда считался
                        // get_chunk_coords_for_world (деление) + get_chunk (hash-lookup),
                        // даже когда сосед гарантированно лежит в том же чанке (lx/lz не на
                        // краю [0, 31]). step_cursor делает это условно — для внутренних
                        // вокселей (подавляющее большинство, 30*30 из 32*32 на слой) это
                        // просто чтение того же chunk по локальным координатам ±1, без
                        // единого обращения к unordered_map.
                        bool is_boundary = false;
                        static constexpr int hdx[4] = {1, -1, 0, 0};
                        static constexpr int hdz[4] = {0, 0, 1, -1};
                        for (int d = 0; d < 4; ++d) {
                            const Voxel_Cursor nb = step_cursor(here, hdx[d], 0, hdz[d], *this);
                            if (!nb.chunk) continue;
                            const Block_ID nid = static_cast<Block_ID>(nb.chunk->get_block(nb.lx, nb.ly, nb.lz));
                            if (transparent[nid] && nb.chunk->get_sky_light(nb.lx, nb.ly, nb.lz) < level - 1) {
                                is_boundary = true;
                                break;
                            }
                        }

                        if (is_boundary) {
                            flat_queue.push({here, level});
                        }
                    }
                }
            }
        }
    }

    while (!flat_queue.empty()) {
        const Sky_BFS_Node node = flat_queue.front();
        flat_queue.pop();
        if (node.level <= 1) continue;

        for (int d = 0; d < 6; ++d) {
            const Voxel_Cursor nc = step_cursor(node.at, k_dx6[d], k_dy6[d], k_dz6[d], *this);
            if (!nc.chunk) continue;

            const Block_ID nid = static_cast<Block_ID>(nc.chunk->get_block(nc.lx, nc.ly, nc.lz));
            if (!transparent[nid]) continue;

            const auto new_level = static_cast<uint8_t>(node.level - 1);
            if (new_level <= nc.chunk->get_sky_light(nc.lx, nc.ly, nc.lz)) continue;

            nc.chunk->set_sky_light(nc.lx, nc.ly, nc.lz, new_level);
            flat_queue.push({nc, new_level});
            touched_chunks.insert({nc.cx, nc.cy, nc.cz});
        }
    }

    for (const auto& pos : touched_chunks) {
        if (Chunk* chunk = get_chunk(pos.x, pos.y, pos.z)) {
            chunk->mark_mesh_dirty();
            chunk->compact_light();   // выровнявшийся свет снова занимает 1 байт, а не 32 КБ
        }
    }
    mark_light_neighbors_dirty(cleared_chunks);
    mark_light_neighbors_dirty(touched_chunks);
}

void Chunk_Manager::recompute_lighting(std::optional<glm::ivec3> extra_clear_position) {
    // ПОЛНЫЙ пересчёт (очистить область + BFS заново), а НЕ инкрементальное добавление/снятие
    // света — осознанный выбор, а не недоделка. Правильный инкрементальный алгоритм снятия
    // света (когда источник убрали или загородили) — это отдельный, гораздо более хрупкий BFS
    // "de-propagation" (сравнить значение соседа с тем, что мог бы дать именно этот источник,
    // и решать, снимать его или это вклад другого источника) — легко получить тонкие баги
    // (осиротевший свет, который никогда не гаснет, либо наоборот гаснет там, где не должен).
    // Полный пересчёт медленнее в пересчёте на одну правку, зато НАМНОГО проще и надёжнее, а
    // источники света — редкое событие (игрок время от времени ставит/ломает Sun Stone), так
    // что цена оправдана: пересчёт бежит по ограниченной области (радиус источника + запас),
    // а не по всему загруженному миру.

    // Шаг 1: область, которую нужно ОЧИСТИТЬ перед пересчётом — не только сами чанки
    // источников, но и всё, куда их свет МОГ БЫ дотянуться (иначе снятие/перемещение
    // источника оставит "осиротевший" свет, который никто не потушит).
    constexpr int chunk_margin = Config::max_light_level / Config::chunk_size + 2;
    std::unordered_set<glm::ivec3, Chunk_Key_Hash> affected_chunks;
    for (const auto& source_pos : m_light_sources) {
        const glm::ivec3 source_chunk = get_chunk_coords_for_world(source_pos.x, source_pos.y, source_pos.z);
        for (int dx = -chunk_margin; dx <= chunk_margin; ++dx) {
            for (int dy = -chunk_margin; dy <= chunk_margin; ++dy) {
                for (int dz = -chunk_margin; dz <= chunk_margin; ++dz) {
                    affected_chunks.insert({source_chunk.x + dx, source_chunk.y + dy, source_chunk.z + dz});
                }
            }
        }
    }
    // Источник, который только что убрали (см. комментарий у объявления в .h) — его уже нет
    // в m_light_sources к этому моменту, поэтому без явного добавления его окрестность
    // осталась бы вне области очистки, и его старый свет никогда бы не погас.
    if (extra_clear_position) {
        const glm::ivec3 source_chunk = get_chunk_coords_for_world(extra_clear_position->x, extra_clear_position->y, extra_clear_position->z);
        for (int dx = -chunk_margin; dx <= chunk_margin; ++dx) {
            for (int dy = -chunk_margin; dy <= chunk_margin; ++dy) {
                for (int dz = -chunk_margin; dz <= chunk_margin; ++dz) {
                    affected_chunks.insert({source_chunk.x + dx, source_chunk.y + dy, source_chunk.z + dz});
                }
            }
        }
    }

    for (const auto& pos : affected_chunks) {
        if (Chunk* chunk = get_chunk(pos.x, pos.y, pos.z)) {
            chunk->clear_block_light();
        }
    }

    // Шаг 2: многоисточниковый BFS. touched_chunks — это то, что РЕАЛЬНО задел проход (может
    // быть меньше affected_chunks, если свет уткнулся в стены раньше, чем достиг границы
    // запаса) — именно это, а не affected_chunks, определяет, какие чанки пересобирать.
    //
    // Раньше узел очереди хранил только мировые координаты и на каждого из 6 соседей заново
    // проделывал get_block_world/get_light_world/set_light_world — а это ТРИ отдельных прохода
    // "мировые координаты -> chunk-координаты (деление) -> get_chunk (hash-map) -> локальные
    // координаты" на одного соседа (по одному на блок, на чтение света, на запись света).
    // Voxel_Cursor + step_cursor (см. анонимный namespace выше) держат chunk-координаты,
    // локальные координаты и указатель на Chunk уже разложенными и лезут в hash-map только
    // при реальном пересечении границы чанка — см. подробный комментарий у step_cursor.
    // Так же заменяем get_block_props(...).is_transparent/.light_emission на плоские таблицы
    // Block_Registry (см. комментарий у их объявления в Block_Types.h).
    const bool* transparent = Block_Registry::get_instance().transparent_table();
    const uint8_t* emission = Block_Registry::get_instance().light_emission_table();

    struct Light_BFS_Node { Voxel_Cursor at; uint8_t level; };
    Flat_Queue<Light_BFS_Node> flat_queue;
    std::unordered_set<glm::ivec3, Chunk_Key_Hash> touched_chunks;

    for (const auto& source_pos : m_light_sources) {
        // Разложение мировых координат на (chunk, local) для самого источника — деление тут
        // не страшно: источников света на весь мир единицы, это не горячий цикл.
        const glm::ivec3 c = get_chunk_coords_for_world(source_pos.x, source_pos.y, source_pos.z);
        Chunk* chunk = get_chunk(c.x, c.y, c.z);
        if (!chunk) continue;
        const int lx = source_pos.x - c.x * Config::chunk_size;
        const int ly = source_pos.y - c.y * Config::chunk_height;
        const int lz = source_pos.z - c.z * Config::chunk_size;

        const Block_ID id = static_cast<Block_ID>(chunk->get_block(lx, ly, lz));
        const int level = emission[id];
        if (level <= 0) continue; // блок под этой позицией больше не источник света
        const auto level_u8 = static_cast<uint8_t>(level);

        chunk->set_light(lx, ly, lz, level_u8);
        flat_queue.push({Voxel_Cursor{c.x, c.y, c.z, lx, ly, lz, chunk}, level_u8});
        touched_chunks.insert(c);
    }

    while (!flat_queue.empty()) {
        const Light_BFS_Node node = flat_queue.front();
        flat_queue.pop();
        if (node.level <= 1) continue; // на уровне 1 распространяться дальше некуда (дало бы 0)

        for (int d = 0; d < 6; ++d) {
            const Voxel_Cursor nc = step_cursor(node.at, k_dx6[d], k_dy6[d], k_dz6[d], *this);
            if (!nc.chunk) continue; // сосед в чанке, который ещё не загружен

            const Block_ID nid = static_cast<Block_ID>(nc.chunk->get_block(nc.lx, nc.ly, nc.lz));
            if (!transparent[nid]) continue; // свет не проходит сквозь непрозрачное

            const auto new_level = static_cast<uint8_t>(node.level - 1);
            if (new_level <= nc.chunk->get_light(nc.lx, nc.ly, nc.lz)) continue; // тут уже не темнее

            nc.chunk->set_light(nc.lx, nc.ly, nc.lz, new_level);
            touched_chunks.insert({nc.cx, nc.cy, nc.cz});
            flat_queue.push({nc, new_level});
        }
    }

    // Шаг 3: и очищенные, и реально задетые чанки — на пересборку меша (см. rebuild_dirty_chunks,
    // вызывается каждый кадр из update_player_position и уже само разберётся асинхронно).
    for (const auto& pos : affected_chunks) {
        if (Chunk* chunk = get_chunk(pos.x, pos.y, pos.z)) {
            chunk->mark_mesh_dirty();
            chunk->compact_light();   // выровнявшийся свет снова занимает 1 байт, а не 32 КБ
        }
    }
    for (const auto& pos : touched_chunks) {
        if (Chunk* chunk = get_chunk(pos.x, pos.y, pos.z)) {
            chunk->mark_mesh_dirty();
            chunk->compact_light();   // выровнявшийся свет снова занимает 1 байт, а не 32 КБ
        }
    }
    mark_light_neighbors_dirty(affected_chunks);
    mark_light_neighbors_dirty(touched_chunks);
}

void Chunk_Manager::mark_light_neighbors_dirty(
    const std::unordered_set<glm::ivec3, Chunk_Key_Hash>& changed_chunks) {
    // Грань соседа смотрит в клетку этого чанка и берёт её свет. Если свет здесь сменился, у соседа
    // граница держала бы старое значение (устаревший свет на стыке), поэтому шесть соседей — грязные.
    static constexpr glm::ivec3 kFaceOffsets[6] = {
        {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    for (const auto& pos : changed_chunks) {
        for (const auto& offset : kFaceOffsets) {
            if (Chunk* neighbor = get_chunk(pos.x + offset.x, pos.y + offset.y, pos.z + offset.z)) {
                neighbor->mark_mesh_dirty();
            }
        }
    }
}

namespace {
// Отсечение укрытий по связности граничных позиций секций (см. Section_Connectivity). Узел обхода — компонента
// прозрачных клеток секции, касающаяся её границы (у Open-секции одна на всю границу). Переход в соседнюю секцию
// идёт только через совпадающие позиции общей грани, поэтому полости, закрытые камнем, не склеиваются с воздухом
// камеры. Секция видна, если в неё вошли или к ней примыкает достижимый воздух: её грань, обращённая к воздуху,
// рисуется, даже если сама секция непроходима. Выход из секции ограничен монотонным правилом (см. leave_ok): путь
// от камеры не возвращается к её плоскости по оси, как и прямой луч.
// Грани: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z; противоположная — f ^ 1.
// Обход идёт по сетке чанков загруженной области с рамкой в один чанк: соседи находятся сдвигом индекса, без
// делений и проверок границ. Рамка и чанки вне области не разворачиваются, но касание к ним учитывается.
constexpr int kReachSections = Config::chunk_section_count;
// Потолок размера сетки в чанках (с рамкой): при большей загруженной области отсечение выключается.
constexpr int64_t kReachMaxChunks = int64_t{1} << 16;

// Связность секции вне загруженной области: через неё не проходят (Closed), но её грань видна от воздуха.
const Chunk_Meshes& outside_meshes() {
    static const Chunk_Meshes meshes = [] {
        Chunk_Meshes m;
        for (Chunk_Section_Range& range : m.sections) range.conn.kind = Section_Connectivity::Kind::Closed;
        return m;
    }();
    return meshes;
}

// Состояние обхода. Живёт между кадрами: поколения (gen) вместо очистки массивов на каждый кадр. Вместе с ним
// хранится ключ кэша: результат зависит только от секции камеры и состояния мира (набор чанков, версии мешей).
struct Reach_Scratch {
    struct Node {
        int sec;
        int comp;
    };
    uint32_t gen = 0;
    glm::ivec3 origin{0};                 // минимальный чанк области
    int dim_x = 0, dim_y = 0, dim_z = 0;  // размер сетки с рамкой, в чанках
    // По чанкам сетки: nullptr — чанка нет внутри области (открыто), outside_meshes() — рамка.
    std::vector<const Chunk_Meshes*> chunks;
    std::vector<uint32_t> reached_gen;  // == gen: секция касается достижимого воздуха или посещена
    std::vector<uint32_t> visit_gen;    // == gen: маска visit_bits секции актуальна
    std::vector<uint64_t> visit_bits;   // 256 бит на секцию: посещённые компоненты
    std::vector<Node> queue;
    // Ключ кэша и его результат (enabled: отсечение применяется).
    bool key_valid = false;
    bool key_enabled = false;
    const void* key_owner = nullptr;
    uint64_t key_set_epoch = 0;
    uint64_t key_conn_epoch = 0;
    glm::ivec3 key_camera_chunk{0};
    int key_camera_section = 0;

    int chunk_index(int x, int y, int z) const {
        return ((y - origin.y + 1) * dim_z + (z - origin.z + 1)) * dim_x + (x - origin.x + 1);
    }
};

// Начало кадра: размеры массивов и новое поколение; очередь пуста.
void begin_frame(Reach_Scratch& s, size_t sections) {
    if (s.reached_gen.size() < sections) {
        s.reached_gen.assign(sections, 0);
        s.visit_gen.assign(sections, 0);
        s.visit_bits.assign(sections * 4, 0);
    }
    if (++s.gen == 0) {  // переполнение поколения (через 2^32 кадров): сбрасываем метки
        std::fill(s.reached_gen.begin(), s.reached_gen.end(), uint32_t{0});
        std::fill(s.visit_gen.begin(), s.visit_gen.end(), uint32_t{0});
        s.gen = 1;
    }
    s.queue.clear();
}

// Отмечает компоненту comp секции посещённой; false — она уже посещена в этом кадре.
bool visit(Reach_Scratch& s, int sec, int comp) {
    const size_t i = static_cast<size_t>(sec);
    if (s.visit_gen[i] != s.gen) {
        s.visit_gen[i] = s.gen;
        std::fill_n(s.visit_bits.begin() + static_cast<std::ptrdiff_t>(i * 4), 4, uint64_t{0});
    }
    uint64_t& word = s.visit_bits[i * 4 + (static_cast<size_t>(comp) >> 6)];
    const uint64_t bit = uint64_t{1} << (comp & 63);
    if (word & bit) return false;
    word |= bit;
    return true;
}

// Связность секции: нет чанка внутри области — открыта; рамка — закрыта (см. outside_meshes).
const Section_Connectivity& connectivity_of(const Chunk_Meshes* chunk, int section) {
    static const Section_Connectivity open{};
    return chunk ? chunk->sections[section].conn : open;
}

// Есть ли общий бит у двух масок граней (одинаковая локальная индексация позиций соседних граней).
bool masks_overlap(const uint64_t* a, const uint64_t* b, int words) {
    for (int w = 0; w < words; ++w) {
        if (a[w] & b[w]) return true;
    }
    return false;
}

// Маска позиций компоненты comp на грани f; nullptr — компонента грани не касается.
const uint64_t* face_mask(const Section_Connectivity& k, int f, int comp) {
    for (uint16_t i = k.face_begin[f]; i < k.face_begin[f + 1]; ++i) {
        if (k.face_comps[i].comp == comp) return &k.masks[k.face_comps[i].mask];
    }
    return nullptr;
}

// Полный пересчёт достижимости от секции камеры (camera_chunk, camera_section) в s. false — отсечение не
// применяется (камера вне области, в запертой секции или область слишком большая): рисуется всё, как раньше.
bool compute_section_reach(const std::vector<std::pair<glm::ivec3, Chunk*>>& loaded,
                           const glm::ivec3& camera_chunk, int camera_section, Reach_Scratch& s) {
    if (loaded.empty()) return false;
    glm::ivec3 lo = loaded.front().first;
    glm::ivec3 hi = lo;
    for (const auto& entry : loaded) {
        lo = glm::min(lo, entry.first);
        hi = glm::max(hi, entry.first);
    }
    const glm::ivec3 box = hi - lo + 1;
    const int64_t chunk_count = int64_t{box.x + 2} * (box.y + 2) * (box.z + 2);
    if (chunk_count > kReachMaxChunks) return false;

    // Камера вне загруженной области: отсечение не работает.
    const glm::ivec3 rel = camera_chunk - lo;
    if (rel.x < 0 || rel.y < 0 || rel.z < 0 || rel.x >= box.x || rel.y >= box.y || rel.z >= box.z) return false;

    s.origin = lo;
    s.dim_x = box.x + 2;
    s.dim_y = box.y + 2;
    s.dim_z = box.z + 2;
    begin_frame(s, static_cast<size_t>(chunk_count) * kReachSections);

    // Рамка закрыта; внутри области чанк по умолчанию отсутствует (открыто), затем — загруженные чанки.
    s.chunks.assign(static_cast<size_t>(chunk_count), &outside_meshes());
    for (int y = lo.y; y <= hi.y; ++y)
        for (int z = lo.z; z <= hi.z; ++z)
            for (int x = lo.x; x <= hi.x; ++x)
                s.chunks[s.chunk_index(x, y, z)] = nullptr;
    for (const auto& entry : loaded) {
        const glm::ivec3& k = entry.first;
        s.chunks[s.chunk_index(k.x, k.y, k.z)] = &entry.second->get_meshes();
    }
    const int camera_index = s.chunk_index(camera_chunk.x, camera_chunk.y, camera_chunk.z);
    // Камера в секции без проходимых граничных клеток: с неё не видно, что за секцией, отсечение выключено.
    if (connectivity_of(s.chunks[camera_index], camera_section).kind == Section_Connectivity::Kind::Closed) {
        return false;
    }

    // Сдвиги индекса секции к соседям: чанк ±X — на 4, ±Z — на строку сетки, ±Y — на слой; секции внутри чанка
    // соседствуют по Y на 1, а переход на соседний чанк по Y — на начало его секции 0 (или 3).
    const int stride_x = kReachSections;
    const int stride_z = kReachSections * s.dim_x;
    const int stride_y = kReachSections * s.dim_x * s.dim_z;
    const int top = kReachSections - 1;
    const auto neighbor = [&](int sec, int f) -> int {
        const int sub = sec % kReachSections;
        switch (f) {
            case 0: return sec + stride_x;
            case 1: return sec - stride_x;
            case 2: return sub < top ? sec + 1 : sec + stride_y - top;
            case 3: return sub > 0 ? sec - 1 : sec - stride_y + top;
            case 4: return sec + stride_z;
            default: return sec - stride_z;  // 5
        }
    };
    const auto connectivity = [&](int sec) -> const Section_Connectivity& {
        return connectivity_of(s.chunks[sec / kReachSections], sec % kReachSections);
    };
    const auto touch = [&](int sec) { s.reached_gen[sec] = s.gen; };
    // Узел (секция, компонента) обрабатывается один раз; вход в секцию делает её видимой.
    const auto push = [&](int sec, int comp) {
        if (!visit(s, sec, comp)) return;
        s.reached_gen[sec] = s.gen;
        s.queue.push_back({sec, comp});
    };
    // Вход через всю грань g: у нас все позиции этой грани проходимы, поэтому входим в каждую компоненту соседа,
    // которая касается грани g; воздух соседа (Open) — одним входом.
    const auto enter_all = [&](int sec, const Section_Connectivity& k, int g) {
        if (k.kind == Section_Connectivity::Kind::Open) {
            push(sec, 0);
        } else if (k.kind == Section_Connectivity::Kind::Mixed) {
            for (uint16_t i = k.face_begin[g]; i < k.face_begin[g + 1]; ++i) push(sec, k.face_comps[i].comp);
        }
    };

    // Монотонность: прямой луч из глаза движется по каждой оси в одну сторону, поэтому путь, по которому видно,
    // не возвращается к плоскости камеры. Выход из секции через грань f разрешён, если по её оси секция не лежит
    // с той стороны от камеры, куда ведёт грань (смещение 0 — по обе стороны). Смещения — по индексам секций.
    const int dims_xz = s.dim_x * s.dim_z;
    const int cam_px = camera_index % s.dim_x;
    const int cam_pz = (camera_index / s.dim_x) % s.dim_z;
    const int cam_ys = (camera_index / dims_xz) * kReachSections + camera_section;
    const auto leave_ok = [&](int sec, int f) -> bool {
        const int ci = sec / kReachSections;
        const int sub = sec % kReachSections;
        int off = 0;
        switch (f >> 1) {  // 0: ось X (грани 0, 1), 1: Y (грани 2, 3), 2: Z (грани 4, 5)
            case 0: off = ci % s.dim_x - cam_px; break;
            case 1: off = (ci / dims_xz) * kReachSections + sub - cam_ys; break;
            default: off = (ci / s.dim_x) % s.dim_z - cam_pz; break;
        }
        const int dir = (f & 1) ? -1 : +1;  // чётные грани ведут в +, нечётные — в -
        const int side = (off > 0) - (off < 0);
        return side == 0 || side == dir;
    };

    // Старт: камера внутри своей секции. Open — вся секция; Mixed — все её компоненты, касающиеся границы
    // (клетки камеры по компонентам не различаем: это верхняя граница видимости).
    const int start = camera_index * kReachSections + camera_section;
    touch(start);
    const Section_Connectivity& cam = connectivity(start);
    if (cam.kind == Section_Connectivity::Kind::Open) {
        push(start, 0);
    } else {
        for (int comp = 0; comp < cam.comp_count; ++comp) push(start, comp);
    }

    for (size_t head = 0; head < s.queue.size(); ++head) {
        const Reach_Scratch::Node node = s.queue[head];
        const Section_Connectivity& c = connectivity(node.sec);
        if (c.kind == Section_Connectivity::Kind::Open) {
            for (int f = 0; f < 6; ++f) {
                if (!leave_ok(node.sec, f)) continue;
                const int n = neighbor(node.sec, f);
                touch(n);
                enter_all(n, connectivity(n), f ^ 1);
            }
        } else {
            // Компонента касается граней, на которых есть её позиции. За каждой такой гранью — сосед: касание
            // к нему есть всегда, а вход — в компоненты соседа, у которых есть позиция на той же локальной
            // позиции грани (пересечение масок).
            for (int f = 0; f < 6; ++f) {
                const uint64_t* mine = face_mask(c, f, node.comp);
                if (!mine || !leave_ok(node.sec, f)) continue;
                const int n = neighbor(node.sec, f);
                touch(n);
                const Section_Connectivity& nc = connectivity(n);
                if (nc.kind == Section_Connectivity::Kind::Open) {
                    push(n, 0);
                } else if (nc.kind == Section_Connectivity::Kind::Mixed) {
                    for (uint16_t i = nc.face_begin[f ^ 1]; i < nc.face_begin[(f ^ 1) + 1]; ++i) {
                        const Section_Connectivity::Face_Comp& e = nc.face_comps[i];
                        if (masks_overlap(mine, &nc.masks[e.mask], Section_Boundary::kFaceWords[f])) {
                            push(n, e.comp);
                        }
                    }
                }
            }
        }
    }
    return true;
}

// Достижимость от камеры. Результат кэшируется: пока секция камеры, набор чанков и связность не менялись, обход
// не пересчитывается (в том числе при движении внутри секции и при взгляде по сторонам).
bool build_section_reach(const std::vector<std::pair<glm::ivec3, Chunk*>>& loaded,
                         const glm::vec3& camera_position, uint64_t set_epoch, const void* owner,
                         Reach_Scratch& s) {
    const glm::ivec3 camera_chunk(
        static_cast<int>(std::floor(camera_position.x / Config::chunk_size)),
        static_cast<int>(std::floor(camera_position.y / Config::chunk_height)),
        static_cast<int>(std::floor(camera_position.z / Config::chunk_size)));
    const int local_y = static_cast<int>(std::floor(camera_position.y)) - camera_chunk.y * Config::chunk_height;
    const int camera_section = std::clamp(local_y / Config::chunk_section_height, 0, kReachSections - 1);

    // Эпоха связности читается до пересчёта: если связность поменяется во время него, ключ устареет.
    const uint64_t conn_epoch = current_connectivity_epoch();
    if (s.key_valid && s.key_owner == owner && s.key_set_epoch == set_epoch && s.key_conn_epoch == conn_epoch &&
        s.key_camera_chunk == camera_chunk && s.key_camera_section == camera_section) {
        return s.key_enabled;
    }
    s.key_valid = true;
    s.key_owner = owner;
    s.key_set_epoch = set_epoch;
    s.key_conn_epoch = conn_epoch;
    s.key_camera_chunk = camera_chunk;
    s.key_camera_section = camera_section;
    s.key_enabled = compute_section_reach(loaded, camera_chunk, camera_section, s);
    return s.key_enabled;
}

// Секция видна от камеры: касается достижимого воздуха или посещена в этом кадре.
bool is_reached(const Reach_Scratch& s, const glm::ivec3& key, int section) {
    const size_t sec = static_cast<size_t>(s.chunk_index(key.x, key.y, key.z)) * kReachSections + section;
    return s.reached_gen[sec] == s.gen;
}
} // namespace

std::vector<Renderable_Chunk> Chunk_Manager::get_renderable_chunks(
    const Frustum* frustum,
    const glm::vec3* camera_position,
    int flora_render_distance_chunks,
    bool spectator
) const {
    std::vector<Renderable_Chunk> result;
    m_visible_chunks.clear();

    // Все загруженные чанки, в том числе пустые: через пустой воздух обзор идёт дальше, и без них
    // обход секций оборвался бы.
    std::vector<std::pair<glm::ivec3, Chunk*>> loaded;
    loaded.reserve(m_chunks.size());
    m_chunks.for_each([&](const glm::ivec3& key, const Chunk_Ptr& chunk) {
        loaded.emplace_back(key, chunk.get());
    });
    // Глаз в непроходимой клетке (камень: /tp в стену, застревание): обзора из воздуха нет, отсечение здесь не
    // применяется, и кадр рисуется как без него. Иначе кадр менялся бы: дальние поверхности, видные сквозь камень,
    // пропадали бы, а это уже видимое на экране.
    bool eye_in_solid = false;
    if (camera_position) {
        const int wx = static_cast<int>(std::floor(camera_position->x));
        const int wy = static_cast<int>(std::floor(camera_position->y));
        const int wz = static_cast<int>(std::floor(camera_position->z));
        const int cx = Math_Helpers::floor_div(wx, Config::chunk_size);
        const int cy = Math_Helpers::floor_div(wy, Config::chunk_height);
        const int cz = Math_Helpers::floor_div(wz, Config::chunk_size);
        if (const Chunk* eye_chunk = get_chunk(cx, cy, cz)) {
            eye_in_solid = block_occludes_view(eye_chunk->get_block(
                wx - cx * Config::chunk_size, wy - cy * Config::chunk_height, wz - cz * Config::chunk_size));
        }
    }
    // Достижимость от камеры; состояние обхода переиспользуется между кадрами (см. Reach_Scratch).
    static thread_local Reach_Scratch reach;
    // В режиме наблюдателя камера проходит сквозь стены, поэтому геометрия за
    // непрозрачными секциями тоже должна оставаться видимой.
    const bool reach_enabled = !spectator && camera_position && !eye_in_solid &&
        build_section_reach(loaded, *camera_position, m_chunk_set_epoch, this, reach);

    for (const auto& [key, chunk] : loaded) {
        if (chunk->is_empty()) continue;
        // Захват области рисует только чанки плана (обычные чанки вокруг игрока, лежащие
        // вне плана, — глубокое подземелье/небо — в кадр не попадают).
        if (capture_active() && !capture_plan_contains(key)) continue;

        const Chunk_Meshes& meshes = chunk->get_meshes();
        Renderable_Chunk renderable{key, &meshes, chunk->get_mesh_version()};
        const glm::vec3 footprint_min(
            static_cast<float>(key.x * Config::chunk_size),
            static_cast<float>(key.y * Config::chunk_height),
            static_cast<float>(key.z * Config::chunk_size)
        );
        const glm::vec3 footprint_max(
            footprint_min.x + static_cast<float>(Config::chunk_size),
            0.0f,
            footprint_min.z + static_cast<float>(Config::chunk_size)
        );

        bool has_visible_section = false;
        for (size_t section_index = 0; section_index < meshes.sections.size(); ++section_index) {
            const Chunk_Section_Range& section = meshes.sections[section_index];
            if (!section.has_geometry) continue;

            const glm::vec3 section_min(footprint_min.x, section.min_y, footprint_min.z);
            const glm::vec3 section_max(footprint_max.x, section.max_y, footprint_max.z);
            const bool in_frustum = !frustum || frustum->intersects_aabb(section_min, section_max);
            const bool reached = !reach_enabled || is_reached(reach, key, static_cast<int>(section_index));
            // Сначала секция неактивна; включаем её, если она попадает во фрустум.
            // Для спектатора обход связности выключен, но проверка поля зрения остаётся.
            const bool visible = in_frustum && (spectator || reached);
            renderable.section_visible[section_index] = visible;
            has_visible_section = has_visible_section || visible;
        }
        if (!has_visible_section) continue;

        if (camera_position && flora_render_distance_chunks >= 0) {
            const int camera_chunk_x = static_cast<int>(
                std::floor(camera_position->x / static_cast<float>(Config::chunk_size)));
            const int camera_chunk_z = static_cast<int>(
                std::floor(camera_position->z / static_cast<float>(Config::chunk_size)));
            const int chunk_dx = std::abs(key.x - camera_chunk_x);
            const int chunk_dz = std::abs(key.z - camera_chunk_z);
            renderable.flora_skip_render =
                chunk_dx > flora_render_distance_chunks || chunk_dz > flora_render_distance_chunks;
        }

        result.push_back(renderable);
        m_visible_chunks.insert(key.x, key.y, key.z);
    }

    return result;
}

// ============================================================================
//  Захват области (изометрический снимок)
// ============================================================================
bool Chunk_Manager::capture_plan_contains(const glm::ivec3& position) const {
    if (m_capture.top_cy.empty()) return false;
    const int dx = position.x - (m_capture.center_cx - m_capture.radius);
    const int dz = position.z - (m_capture.center_cz - m_capture.radius);
    if (dx < 0 || dz < 0 || dx >= m_capture.side || dz >= m_capture.side) return false;
    const size_t index = static_cast<size_t>(dz) * static_cast<size_t>(m_capture.side) + static_cast<size_t>(dx);
    return position.y >= m_capture.bottom_cy[index] && position.y <= m_capture.top_cy[index];
}

Chunk_Manager::Capture_Clip Chunk_Manager::current_capture_clip() const {
    Capture_Clip clip;
    if (capture_active()) {
        clip.active = true;
        clip.min_cx = m_capture.center_cx - m_capture.radius;
        clip.max_cx = m_capture.center_cx + m_capture.radius;
        clip.min_cz = m_capture.center_cz - m_capture.radius;
        clip.max_cz = m_capture.center_cz + m_capture.radius;
    }
    return clip;
}

void Chunk_Manager::begin_capture_region(int center_cx, int center_cz, int radius,
                                         int extra_depth_layers, int top_margin_blocks) {
    if (!m_world_generator) {
        LOG_ERROR("Chunk_Manager::begin_capture_region: World_Generator is not set");
        return;
    }
    m_capture = Capture_Region{};
    m_capture.phase = static_cast<int>(Capture_Phase::Planning);
    m_capture.center_cx = center_cx;
    m_capture.center_cz = center_cz;
    m_capture.radius = radius;
    m_capture.extra_depth = std::max(0, extra_depth_layers);
    m_capture.top_margin = std::max(0, top_margin_blocks);
    m_capture.side = 2 * radius + 1;
    m_capture.apron_side = m_capture.side + 2;
    const size_t apron_cells = static_cast<size_t>(m_capture.apron_side) * static_cast<size_t>(m_capture.apron_side);
    m_capture.col_min_h.assign(apron_cells, 0);
    m_capture.col_max_h.assign(apron_cells, 0);
    m_capture.planned_columns = 0;
    LOG_INFO("Capture region: planning " + std::to_string(m_capture.side) + "x" +
             std::to_string(m_capture.side) + " chunk columns");
}

void Chunk_Manager::capture_finish_planning() {
    const int side = m_capture.side;
    const int apron = m_capture.apron_side;
    const int min_cy = Config::world_min_chunk_y;
    const int max_cy = Config::world_max_chunk_y;
    constexpr int sea_level = World_Generator::sea_level();

    m_capture.bottom_cy.assign(static_cast<size_t>(side) * side, 0);
    m_capture.top_cy.assign(static_cast<size_t>(side) * side, 0);
    m_capture.chunks_total = 0;

    // Общий плоский низ среза (как «кусок земли» в Indev): самая низкая точка рельефа области.
    int global_lowest = std::numeric_limits<int>::max();
    for (int v : m_capture.col_min_h) global_lowest = std::min(global_lowest, v);
    const int flat_bottom = floor_div_chunk(global_lowest, Config::chunk_height) - m_capture.extra_depth;

    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            // Колонка (x,z) области лежит в (x+1, z+1) таблицы с кольцом.
            int lowest = std::numeric_limits<int>::max();
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const size_t a = static_cast<size_t>(z + 1 + dz) * apron + static_cast<size_t>(x + 1 + dx);
                    lowest = std::min(lowest, m_capture.col_min_h[a]);
                }
            }
            const size_t own = static_cast<size_t>(z + 1) * apron + static_cast<size_t>(x + 1);
            const int highest = std::max(m_capture.col_max_h[own], sea_level);

            // Верх — рельеф (или вода) плюс запас на деревья; низ — самая низкая точка этой
            // колонки и её соседей (иначе у обрыва у соседа были бы видны «дыры») плюс
            // дополнительные слои для толщины среза.
            int top = floor_div_chunk(highest + m_capture.top_margin, Config::chunk_height);
            int bottom = flat_bottom; (void)lowest;
            top = std::clamp(top, min_cy, max_cy);
            bottom = std::clamp(bottom, min_cy, top);

            const size_t index = static_cast<size_t>(z) * side + static_cast<size_t>(x);
            m_capture.bottom_cy[index] = bottom;
            m_capture.top_cy[index] = top;
            m_capture.chunks_total += static_cast<size_t>(top - bottom + 1);
        }
    }

    if (std::getenv("OPTICRAFT_ISO_DEBUG")) {
        for (int z = 0; z < side; ++z) {
            std::string row;
            for (int x = 0; x < side; ++x) {
                const size_t index = static_cast<size_t>(z) * side + static_cast<size_t>(x);
                row += "[" + std::to_string(m_capture.bottom_cy[index]) + ".." + std::to_string(m_capture.top_cy[index]) + "] ";
            }
            LOG_INFO("plan z=" + std::to_string(z) + ": " + row);
        }
    }

    // Ставим в очередь все чанки плана. Уже загруженные enqueue_chunk_load пропускает сам.
    const int origin_cx = m_capture.center_cx - m_capture.radius;
    const int origin_cz = m_capture.center_cz - m_capture.radius;
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            const size_t index = static_cast<size_t>(z) * side + static_cast<size_t>(x);
            const bool on_edge = x == 0 || z == 0 || x == side - 1 || z == side - 1;
            for (int cy = m_capture.bottom_cy[index]; cy <= m_capture.top_cy[index]; ++cy) {
                const int cx = origin_cx + x;
                const int cz = origin_cz + z;
                if (is_chunk_loaded(cx, cy, cz)) {
                    // Меш такого чанка был построен без учёта границы кадра — пересобрать.
                    if (on_edge) {
                        if (Chunk* existing = get_chunk(cx, cy, cz)) existing->mark_mesh_dirty();
                    }
                } else {
                    enqueue_chunk_load(cx, cy, cz);
                }
            }
        }
    }

    m_capture.phase = static_cast<int>(Capture_Phase::Generating);
    LOG_INFO("Capture region: " + std::to_string(m_capture.chunks_total) + " chunks planned");
}

void Chunk_Manager::pump_capture_region() {
    if (!capture_active()) return;
    const Capture_Phase phase = static_cast<Capture_Phase>(m_capture.phase);

    if (phase == Capture_Phase::Planning) {
        // Высоты считаем частями по бюджету времени, чтобы окно не зависало: на колонку
        // приходится 25 запросов к генератору.
        const int apron = m_capture.apron_side;
        const size_t total = static_cast<size_t>(apron) * static_cast<size_t>(apron);
        const auto start = std::chrono::steady_clock::now();
        static constexpr int probes[5] = {0, 8, 16, 24, Config::chunk_size - 1};

        while (m_capture.planned_columns < total) {
            const size_t index = m_capture.planned_columns;
            const int ax = static_cast<int>(index % static_cast<size_t>(apron));
            const int az = static_cast<int>(index / static_cast<size_t>(apron));
            // Индекс таблицы с кольцом -> чанковая колонка: ax=0 это колонка origin-1.
            const int cx = m_capture.center_cx - m_capture.radius - 1 + ax;
            const int cz = m_capture.center_cz - m_capture.radius - 1 + az;

            int lo = std::numeric_limits<int>::max();
            int hi = std::numeric_limits<int>::min();
            for (const int pz : probes) {
                for (const int px : probes) {
                    const int h = m_world_generator->get_height(cx * Config::chunk_size + px,
                                                                cz * Config::chunk_size + pz);
                    lo = std::min(lo, h);
                    hi = std::max(hi, h);
                }
            }
            m_capture.col_min_h[index] = lo;
            m_capture.col_max_h[index] = hi;
            ++m_capture.planned_columns;

            const double elapsed_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed_ms > capture_plan_budget_ms) break;
        }
        if (m_capture.planned_columns >= total) capture_finish_planning();
        return;
    }

    if (phase == Capture_Phase::Generating) {
        collect_finished_chunks();
        size_t loaded = 0;
        const int side = m_capture.side;
        const int origin_cx = m_capture.center_cx - m_capture.radius;
        const int origin_cz = m_capture.center_cz - m_capture.radius;
        for (int z = 0; z < side; ++z) {
            for (int x = 0; x < side; ++x) {
                const size_t index = static_cast<size_t>(z) * side + static_cast<size_t>(x);
                for (int cy = m_capture.bottom_cy[index]; cy <= m_capture.top_cy[index]; ++cy) {
                    if (is_chunk_loaded(origin_cx + x, cy, origin_cz + z)) ++loaded;
                }
            }
        }
        if (loaded >= m_capture.chunks_total) {
            m_capture.lighting_columns_total = m_columns_pending_sky_recompute.size();
            m_capture.phase = static_cast<int>(Capture_Phase::Lighting);
        }
        return;
    }

    if (phase == Capture_Phase::Lighting) {
        // Небесный свет — пачками по колонкам; каждая пачка помечает затронутые меши грязными.
        std::unordered_set<uint64_t> batch;
        for (auto it = m_columns_pending_sky_recompute.begin();
             it != m_columns_pending_sky_recompute.end() && batch.size() < capture_sky_columns_per_frame;) {
            batch.insert(*it);
            it = m_columns_pending_sky_recompute.erase(it);
        }
        recompute_sky_lighting(batch, false);

        if (m_columns_pending_sky_recompute.empty()) {
            if (!m_light_sources.empty()) recompute_lighting();
            // Меши строятся один раз, после того как весь свет посчитан.
            size_t dirty = 0;
            m_chunks.for_each([&](const glm::ivec3& key, const Chunk_Ptr& chunk) {
                if (chunk->is_mesh_dirty() && capture_plan_contains(key)) ++dirty;
            });
            m_capture.dirty_at_meshing_start = std::max<size_t>(dirty, 1);
            m_capture.phase = static_cast<int>(Capture_Phase::Meshing);
        }
        return;
    }

    if (phase == Capture_Phase::Meshing) {
        collect_finished_mesh_jobs();
        rebuild_dirty_chunks();

        size_t dirty = 0;
        m_chunks.for_each([&](const glm::ivec3& key, const Chunk_Ptr& chunk) {
            if (chunk->is_mesh_dirty() && capture_plan_contains(key)) ++dirty;
        });
        if (dirty == 0 && m_pending_mesh_rebuilds.empty() && m_mesh_rebuild_requeue.empty()) {
            if (!m_capture.remesh_pass_done) {
                // Второй проход: все меши плана пересобираются уже с окончательным светом
                // (ранние пересборки соседей могли застать неподсчитанный свет).
                m_capture.remesh_pass_done = true;
                m_chunks.for_each([&](const glm::ivec3& key, const Chunk_Ptr& chunk) {
                    if (capture_plan_contains(key)) chunk->mark_mesh_dirty();
                });
                m_capture.dirty_at_meshing_start = std::max<size_t>(m_capture.chunks_total, 1);
                return;
            }
            m_capture.phase = static_cast<int>(Capture_Phase::Ready);
            LOG_INFO("Capture region: ready");
        }
        return;
    }
}

Chunk_Manager::Capture_Status Chunk_Manager::get_capture_status() const {
    Capture_Status status;
    status.phase = static_cast<Capture_Phase>(m_capture.phase);
    status.chunks_total = m_capture.chunks_total;
    if (!capture_active()) return status;

    switch (status.phase) {
        case Capture_Phase::Planning: {
            const double total = static_cast<double>(m_capture.apron_side) * m_capture.apron_side;
            status.progress = 0.05f * static_cast<float>(static_cast<double>(m_capture.planned_columns) / total);
            break;
        }
        case Capture_Phase::Generating: {
            // Считаем загруженные чанки плана (дёшево: по колонкам области).
            size_t loaded = 0;
            const int side = m_capture.side;
            const int origin_cx = m_capture.center_cx - m_capture.radius;
            const int origin_cz = m_capture.center_cz - m_capture.radius;
            for (int z = 0; z < side; ++z) {
                for (int x = 0; x < side; ++x) {
                    const size_t index = static_cast<size_t>(z) * side + static_cast<size_t>(x);
                    for (int cy = m_capture.bottom_cy[index]; cy <= m_capture.top_cy[index]; ++cy) {
                        if (is_chunk_loaded(origin_cx + x, cy, origin_cz + z)) ++loaded;
                    }
                }
            }
            status.chunks_loaded = loaded;
            const float fraction = m_capture.chunks_total
                ? static_cast<float>(loaded) / static_cast<float>(m_capture.chunks_total) : 1.0f;
            status.progress = 0.05f + 0.60f * fraction;
            break;
        }
        case Capture_Phase::Lighting: {
            const float total = static_cast<float>(std::max<size_t>(m_capture.lighting_columns_total, 1));
            const float left = static_cast<float>(m_columns_pending_sky_recompute.size());
            status.chunks_loaded = m_capture.chunks_total;
            status.progress = 0.65f + 0.05f * std::clamp(1.0f - left / total, 0.0f, 1.0f);
            break;
        }
        case Capture_Phase::Meshing: {
            size_t dirty = 0;
            m_chunks.for_each([&](const glm::ivec3& key, const Chunk_Ptr& chunk) {
                if (chunk->is_mesh_dirty() && capture_plan_contains(key)) ++dirty;
            });
            status.chunks_loaded = m_capture.chunks_total;
            const float fraction = 1.0f - static_cast<float>(dirty) /
                                          static_cast<float>(m_capture.dirty_at_meshing_start);
            status.progress = 0.70f + 0.30f * std::clamp(fraction, 0.0f, 1.0f);
            break;
        }
        case Capture_Phase::Ready:
            status.chunks_loaded = m_capture.chunks_total;
            status.progress = 1.0f;
            break;
        case Capture_Phase::Inactive:
            break;
    }
    return status;
}

void Chunk_Manager::end_capture_region() {
    if (!capture_active()) return;

    const Capture_Clip clip = current_capture_clip();
    m_capture = Capture_Region{};

    // Не начатые задачи генерации области больше не нужны; уже идущие дойдут и будут
    // отброшены is_chunk_wanted, если лежат вне обычного окна вокруг игрока.
    {
        std::lock_guard<std::mutex> lock(m_job_mutex);
        m_pending_jobs.clear();
        m_pending_job_count.store(0);
    }
    m_requested_chunks.clear();

    // Меши, собранные во время захвата, построены с only_positive_faces (клип активен): у них нет
    // граней -X/-Y/-Z, а у граничных чанков ещё и боковые грани обрезаны воздухом. Пересобираем
    // все загруженные чанки; старый меш остаётся видимым до готовности нового, так что дыр не будет.
    // Задачи, которые ещё летели с клипом, при этом получат dirty-эпоху и поставятся заново.
    if (clip.active) {
        m_chunks.for_each([&](const glm::ivec3&, const Chunk_Ptr& chunk) {
            chunk->mark_mesh_dirty();
        });
    }

    // Следующий update_player_position пересчитает и загрузку, и выгрузку под обычное окно:
    // всё, что лежит вне него (срез мира), освободится.
    m_player_chunk = {std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
    LOG_INFO("Capture region: closed");
}

std::vector<glm::ivec3> Chunk_Manager::take_unloaded_chunk_positions() {
    std::vector<glm::ivec3> unloaded;
    unloaded.swap(m_unloaded_since_last_query);
    return unloaded;
}
