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
    start_workers();
}

Chunk_Manager::~Chunk_Manager() {
    stop_workers();
    // Воркеры уже остановлены, поэтому m_chunks трогает только этот поток.
    save_all();
}

void Chunk_Manager::save_all() {
    m_chunks.for_each([this](const glm::ivec3&, const std::unique_ptr<Chunk>& chunk) {
        save_chunk_to_disk(*chunk);
    });
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
        std::priority_queue<Chunk_Job, std::vector<Chunk_Job>, Chunk_Job_Priority> empty;
        std::swap(m_pending_jobs, empty);
        std::priority_queue<Mesh_Job, std::vector<Mesh_Job>, Mesh_Job_Priority> empty_mesh;
        std::swap(m_pending_mesh_jobs, empty_mesh);
    }
    m_job_available.notify_all();

    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_workers.clear();
}

void Chunk_Manager::worker_loop(bool mesh_dedicated) {
    while (true) {
        Chunk_Job job;
        Mesh_Job mesh_job;
        bool has_generation_job = false;
        bool has_mesh_job = false;
        {
            std::unique_lock<std::mutex> lock(m_job_mutex);
            m_job_available.wait(lock, [this, mesh_dedicated] {
                if (m_stop_workers) return true;
                if (mesh_dedicated) return !m_pending_mesh_jobs.empty();
                return !m_pending_mesh_jobs.empty() || !m_pending_jobs.empty();
            });

            if (m_stop_workers) {
                return;
            }

            // Меши всегда имеют абсолютный приоритет над генерацией. Это критично для
            // интерактивности: даже если вокруг ещё строится огромный вертикальный мир,
            // ломание/постановка блока должно получить новый меш как можно быстрее.
            if (!m_pending_mesh_jobs.empty()) {
                mesh_job = std::move(m_pending_mesh_jobs.top());
                m_pending_mesh_jobs.pop();
                has_mesh_job = true;
            } else if (!mesh_dedicated && !m_pending_jobs.empty()) {
                job = m_pending_jobs.top();
                m_pending_jobs.pop();
                has_generation_job = true;
            }
        }

        const World_Generator* generator = m_world_generator;

        if (has_generation_job) {
            if (!generator) {
                continue;
            }

            auto chunk = std::make_unique<Chunk>(job.position.x, job.position.y, job.position.z, *generator);
            chunk->set_texture_atlas(m_texture_atlas);

            // Persistent-world rule: an existing chunk file ALWAYS wins over procedural
            // generation. A missing file means this is genuinely new terrain.
            // Логи убраны намеренно: это тысячи std::endl (каждый со сбросом
            // потока) из нескольких воркеров сразу — и лишняя работа, и точка
            // конкуренции на std::cout при массовой загрузке мира.
            if (m_world_file.load_chunk(job.position.x, job.position.y, job.position.z,
                                        m_generation_folder,
                                        chunk->get_blocks_mutable())) {
                chunk->recalculate_metadata();
                // Чанк только что прочитан с диска — он уже сохранён.
                chunk->clear_modified();
            } else {
                chunk->generate_blocks();
            }
            std::lock_guard<std::mutex> lock(m_completed_mutex);
            m_completed_jobs.push({job.position, job.generation_id, std::move(chunk)});
        } else if (has_mesh_job) {
            if (!m_texture_atlas) {
                continue;
            }

            // Резолвит блок за пределами чанка из снимков соседей, сделанных на главном
            // потоке (см. enqueue_mesh_rebuild). Ровно один из local_x/y/z всегда выходит
            // за диапазон за раз — build_chunk_mesh никогда не спрашивает про диагональных
            // соседей, только про 6 граней, так что достаточно ровно этих 6 снимков.
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

                if (neighbor_index >= 0 && snapshot.neighbor_blocks[neighbor_index]) {
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

            Chunk_Meshes meshes = build_chunk_mesh(mesh_job.own_blocks, mesh_job.own_light,
                                                    mesh_job.position.x, mesh_job.position.y, mesh_job.position.z,
                                                    lookup, light_lookup, *m_texture_atlas,
                                                    mesh_job.clip.active, meta_lookup);

            std::lock_guard<std::mutex> lock(m_completed_mutex);
            m_completed_mesh_jobs.push({mesh_job.position, mesh_job.generation_id, std::move(meshes)});
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

    return m_world_file.save_chunk(chunk.get_chunk_x(), chunk.get_chunk_y(), chunk.get_chunk_z(),
                                   m_generation_folder, chunk.get_blocks());
}

void Chunk_Manager::unload_chunk(int cx, int cy, int cz) {
    if (Chunk* existing = m_chunks.find(cx, cy, cz)) {
        save_chunk_to_disk(*existing);
        m_unloaded_since_last_query.push_back({cx, cy, cz});
        m_chunks.erase(cx, cy, cz);
    }
    m_requested_chunks.erase(cx, cy, cz);
    // Не обязательно для корректности (collect_finished_mesh_jobs и так безопасно
    // отбрасывает результат для выгруженного чанка), но не оставляем мусор в множествах —
    // иначе если на этой же позиции скоро появится новый чанк, enqueue_mesh_rebuild
    // ошибочно решит, что для него уже что-то летит, и молча пропустит постановку задачи.
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

    // Вертикаль весим сильнее горизонтали: игроку важнее сначала получить свой собственный
    // "этаж" мира по всем сторонам, чем далёкое небо/подземелье в двух шагах по горизонтали.
    // Так пользователь видит законченный, стоящий на земле мир вокруг себя быстрее — даже
    // если весь остальной столбец (все 8 чанков по Y, см. load_around) всё равно рано или
    // поздно догрузится следом.
    const int dx = cx - m_player_chunk.x;
    const int dz = cz - m_player_chunk.y;
    const int dy = cy - m_player_chunk_y;
    const int priority = dx * dx + dz * dz + dy * dy * 4;

    {
        std::lock_guard<std::mutex> lock(m_job_mutex);
        m_pending_jobs.push({{cx, cy, cz}, m_generation_id, priority});
    }
    m_job_available.notify_one();
}

void Chunk_Manager::enqueue_mesh_rebuild(int cx, int cy, int cz) {
    const glm::ivec3 position{cx, cy, cz};
    Chunk* target = get_chunk(cx, cy, cz);
    if (!target) {
        return;
    }

    if (m_pending_mesh_rebuilds.contains(cx, cy, cz)) {
        // Уже летит задача для этого чанка — не дублируем работу, а просто просим
        // догнать ещё раз свежими данными, когда текущая задача вернётся.
        m_mesh_rebuild_requeue.insert(cx, cy, cz);
        return;
    }
    m_pending_mesh_rebuilds.insert(cx, cy, cz);

    Mesh_Job job;
    job.position = position;
    job.own_blocks = target->get_blocks();
    job.own_light = target->get_light_grid();
    job.generation_id = m_generation_id;
    job.clip = current_capture_clip();

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
                        block_face.data[idx] = static_cast<uint8_t>(static_cast<Block_ID>(neighbor->get_block(x, y, z)));
                        block_face.meta[idx] = neighbor->get_meta(x, y, z);
                        light_face.data[idx] = neighbor->get_packed_light(x, y, z);
                    }
                }
            } else if (i == 2 || i == 3) {
                const int y = (i == 2) ? Config::chunk_height - 1 : 0;
                for (int z = 0; z < Config::chunk_size; ++z) {
                    for (int x = 0; x < Config::chunk_size; ++x) {
                        const size_t idx = static_cast<size_t>(z) * Config::chunk_size + x;
                        block_face.data[idx] = static_cast<uint8_t>(static_cast<Block_ID>(neighbor->get_block(x, y, z)));
                        block_face.meta[idx] = neighbor->get_meta(x, y, z);
                        light_face.data[idx] = neighbor->get_packed_light(x, y, z);
                    }
                }
            } else {
                const int z = (i == 4) ? Config::chunk_size - 1 : 0;
                for (int y = 0; y < Config::chunk_height; ++y) {
                    for (int x = 0; x < Config::chunk_size; ++x) {
                        const size_t idx = static_cast<size_t>(y) * Config::chunk_size + x;
                        block_face.data[idx] = static_cast<uint8_t>(static_cast<Block_ID>(neighbor->get_block(x, y, z)));
                        block_face.meta[idx] = neighbor->get_meta(x, y, z);
                        light_face.data[idx] = neighbor->get_packed_light(x, y, z);
                    }
                }
            }
            job.neighbor_blocks[i] = std::move(block_face);
            job.neighbor_light[i] = std::move(light_face);
        }
    }

    // Меньше = срочнее. Правка блока получает самый высокий приоритет, затем близкие
    // чанки, затем обычные фоновые пересборки после генерации соседей.
    const int dx = std::abs(cx - m_player_chunk.x);
    const int dz = std::abs(cz - m_player_chunk.y);
    const int dy = std::abs(cy - m_player_chunk_y);
    job.priority = dx * dx + dz * dz + dy * dy * 4;

    {
        std::lock_guard<std::mutex> lock(m_job_mutex);
        m_pending_mesh_jobs.push(std::move(job));
    }
    m_job_available.notify_one();
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
    return dx <= m_load_radius + 1 && dz <= m_load_radius + 1 &&
           dy <= Config::vertical_load_radius_chunks;
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
    const int start_cx = player_cx - m_load_radius;
    const int end_cx = player_cx + m_load_radius;
    const int start_cz = player_cz - m_load_radius;
    const int end_cz = player_cz + m_load_radius;

    for (int cx = start_cx; cx <= end_cx; ++cx) {
        for (int cz = start_cz; cz <= end_cz; ++cz) {
            // Загружаем только вертикальное окно вокруг игрока; при полёте/спуске окно
            // сдвигается, поэтому весь диапазон -256..1024 остаётся доступен без хранения
            // всех 41 вертикальных чанков одновременно.
            const int min_cy = std::max(Config::world_min_chunk_y,
                                        m_player_chunk_y - Config::vertical_load_radius_chunks);
            const int max_cy = std::min(Config::world_max_chunk_y,
                                        m_player_chunk_y + Config::vertical_load_radius_chunks);
            for (int cy = min_cy; cy <= max_cy; ++cy) {
                enqueue_chunk_load(cx, cy, cz);
            }
        }
    }
}

void Chunk_Manager::unload_distant(int center_cx, int center_cz, int max_distance) {
    std::vector<glm::ivec3> to_unload;

    m_chunks.for_each([&](const glm::ivec3& key, const std::unique_ptr<Chunk>&) {
        const int dx = std::abs(key.x - center_cx);
        const int dz = std::abs(key.z - center_cz);
        const int dy = std::abs(key.y - m_player_chunk_y);
        if (dx > max_distance || dz > max_distance ||
            dy > Config::vertical_load_radius_chunks) {
            to_unload.push_back(key);
        }
    });

    for (const auto& key : to_unload) {
        unload_chunk(key.x, key.y, key.z);
    }
}

void Chunk_Manager::update_player_position(float player_x, float player_y, float player_z) {
    // Пока идёт захват области, загрузкой управляет pump_capture_region().
    if (capture_active()) return;

    collect_finished_chunks();
    collect_finished_mesh_jobs();

    const int player_cx = static_cast<int>(std::floor(player_x / Config::chunk_size));
    const int player_cz = static_cast<int>(std::floor(player_z / Config::chunk_size));
    const int player_cy = static_cast<int>(std::floor(player_y / Config::chunk_height));

    const bool horizontal_changed = player_cx != m_player_chunk.x || player_cz != m_player_chunk.y;
    const bool vertical_changed = player_cy != m_player_chunk_y;
    if (!horizontal_changed && !vertical_changed) {
        rebuild_dirty_chunks();
        return;
    }

    m_player_chunk = {player_cx, player_cz};
    m_player_chunk_y = player_cy;

    unload_distant(player_cx, player_cz, m_load_radius + 1);
    load_around(player_cx, player_cz);
    rebuild_dirty_chunks();
}

void Chunk_Manager::collect_finished_chunks() {
    std::queue<Chunk_Result> completed;
    {
        std::lock_guard<std::mutex> lock(m_completed_mutex);
        std::swap(completed, m_completed_jobs);
    }

    bool any_chunk_inserted = false;

    const size_t chunk_cap = capture_active() ? capture_max_finished_chunks_per_frame
                                              : max_finished_chunks_per_frame;
    size_t processed_chunks = 0;
    while (!completed.empty() && processed_chunks < chunk_cap) {
        Chunk_Result result = std::move(completed.front());
        completed.pop();
        ++processed_chunks;

        m_requested_chunks.erase(result.position.x, result.position.y, result.position.z);

        if (result.generation_id != m_generation_id ||
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
        m_columns_pending_sky_recompute.insert(
            (static_cast<uint64_t>(static_cast<uint32_t>(result.position.x)) << 32) |
            static_cast<uint32_t>(result.position.z));

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

        const glm::ivec3 neighbors[] = {
            {result.position.x - 1, result.position.y, result.position.z},
            {result.position.x + 1, result.position.y, result.position.z},
            {result.position.x, result.position.y, result.position.z - 1},
            {result.position.x, result.position.y, result.position.z + 1},
            // Вертикальные соседи по стеку чанков — без этого швы между чанками по Y
            // (например, потолок пещеры на границе секции) не пересчитаются у обеих сторон.
            {result.position.x, result.position.y - 1, result.position.z},
            {result.position.x, result.position.y + 1, result.position.z},
        };

        for (const auto& neighbor_pos : neighbors) {
            if (Chunk* neighbor = get_chunk(neighbor_pos.x, neighbor_pos.y, neighbor_pos.z)) {
                neighbor->mark_mesh_dirty();
            }
        }
    }

    if (!completed.empty()) {
        std::lock_guard<std::mutex> lock(m_completed_mutex);
        while (!completed.empty()) {
            m_completed_jobs.push(std::move(completed.front()));
            completed.pop();
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

    if (!m_columns_pending_sky_recompute.empty()) {
        ++m_frames_since_sky_recompute;
    }
    // "Queue fully drained" is the fast, ideal path for a normal-sized load — but with a
    // large render distance, or a player who keeps walking (load_around keeps handing the
    // queue new edge chunks as fast as it drains old ones), m_requested_chunks may not empty
    // for a very long time, or ever, during a play session. Force a recompute after a bounded
    // number of frames regardless, so newly loaded terrain is never stuck dark indefinitely.
    // This is still cheap even at that cadence because recompute_sky_lighting is scoped to
    // m_columns_pending_sky_recompute (just the columns that changed), not the whole loaded
    // world - an earlier version forced a full-world recompute on this same timer, which at
    // a few times a second is what made the game hang/get killed.
    constexpr int sky_recompute_max_frame_delay = 30;
    if (!m_columns_pending_sky_recompute.empty() &&
        (m_requested_chunks.empty() || m_frames_since_sky_recompute >= sky_recompute_max_frame_delay)) {
        // Обрабатываем небольшой пакет только вертикального света. Пустые чанки
        // пропускаются, а горизонтальный flood-fill отложен до ручных изменений
        // блоков, поэтому пакет не создаёт длинный стоп-кадр.
        constexpr size_t sky_columns_per_frame = 8;
        std::unordered_set<uint64_t> batch;
        for (auto it = m_columns_pending_sky_recompute.begin();
             it != m_columns_pending_sky_recompute.end() &&
             batch.size() < sky_columns_per_frame;) {
            batch.insert(*it);
            it = m_columns_pending_sky_recompute.erase(it);
        }
        recompute_sky_lighting(batch, false);
        m_frames_since_sky_recompute = 0;
        if (!m_light_sources.empty()) {
            recompute_lighting();
        }
    }
}

void Chunk_Manager::collect_finished_mesh_jobs() {
    std::queue<Mesh_Result> completed;
    {
        std::lock_guard<std::mutex> lock(m_completed_mutex);
        std::swap(completed, m_completed_mesh_jobs);
    }

    const size_t mesh_cap = capture_active() ? capture_max_finished_meshes_per_frame
                                             : max_finished_meshes_per_frame;
    size_t processed_meshes = 0;
    while (!completed.empty() && processed_meshes < mesh_cap) {
        Mesh_Result result = std::move(completed.front());
        completed.pop();
        ++processed_meshes;

        m_pending_mesh_rebuilds.erase(result.position.x, result.position.y, result.position.z);

        const bool should_requeue =
            m_mesh_rebuild_requeue.erase(result.position.x, result.position.y, result.position.z);

        // Мир пересоздан (F6/новый seed) уже после того, как эта задача была поставлена —
        // результат относится к чанкам, которых больше нет. Тихо отбрасываем.
        if (result.generation_id != m_generation_id) {
            continue;
        }

        // Чанк успели выгрузить (игрок ушёл далеко), пока задача летала в воркере —
        // применять результат некуда и не к чему, безопасно отбрасываем.
        Chunk* chunk = get_chunk(result.position.x, result.position.y, result.position.z);
        if (!chunk) {
            continue;
        }

        chunk->apply_mesh(std::move(result.meshes));

        // Блок поменялся ещё раз, пока эта пересборка считалась в фоне — снимок, с которым
        // она стартовала, уже устарел. Ставим ещё одну задачу со свежими данными, чтобы
        // не потерять последнюю правку молча.
        if (should_requeue) {
            enqueue_mesh_rebuild(result.position.x, result.position.y, result.position.z);
        }
    }

    if (!completed.empty()) {
        std::lock_guard<std::mutex> lock(m_completed_mutex);
        while (!completed.empty()) {
            m_completed_mesh_jobs.push(std::move(completed.front()));
            completed.pop();
        }
    }
}

void Chunk_Manager::rebuild_dirty_chunks() {
    const size_t enqueue_cap = capture_active() ? capture_max_mesh_jobs_enqueued_per_frame
                                                : max_mesh_jobs_enqueued_per_frame;
    size_t enqueued = 0;
    m_chunks.for_each_until([&](const glm::ivec3& key, std::unique_ptr<Chunk>& chunk) {
        // Пересборка асинхронная (см. enqueue_mesh_rebuild) — старый меш чанка остаётся
        // видимым до готовности нового, флаг is_mesh_dirty снимается в apply_mesh() уже
        // на главном потоке, когда результат придёт. enqueue_mesh_rebuild сам дедуплицирует
        // повторные вызовы для чанка, у которого пересборка уже летит, так что тут можно
        // звать его безусловно на каждый dirty-чанк каждый кадр без риска заспамить очередь.
        if (chunk->is_mesh_dirty()) {
            enqueue_mesh_rebuild(key.x, key.y, key.z);
            if (++enqueued >= enqueue_cap) {
                return false; // хватит на этот кадр
            }
        }
        return true;
    });
}

void Chunk_Manager::clear() {
    ++m_generation_id;
    m_chunks.for_each([this](const glm::ivec3& position, const std::unique_ptr<Chunk>& chunk) {
        save_chunk_to_disk(*chunk);
        m_unloaded_since_last_query.push_back(position);
    });
    // Индекс region-файла живёт в памяти между записями — здесь он уезжает на диск.
    m_world_file.flush_chunks(m_generation_folder);
    m_chunks.clear();
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
        std::priority_queue<Chunk_Job, std::vector<Chunk_Job>, Chunk_Job_Priority> empty;
        std::swap(m_pending_jobs, empty);
        std::priority_queue<Mesh_Job, std::vector<Mesh_Job>, Mesh_Job_Priority> empty_mesh;
        std::swap(m_pending_mesh_jobs, empty_mesh);
    }
    {
        std::lock_guard<std::mutex> lock(m_completed_mutex);
        std::queue<Chunk_Result> empty;
        std::swap(m_completed_jobs, empty);
        std::queue<Mesh_Result> empty_mesh;
        std::swap(m_completed_mesh_jobs, empty_mesh);
    }
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
    enqueue_mesh_rebuild(cx, cy, cz);
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
    enqueue_mesh_rebuild(cx, cy, cz);

    const bool on_x_edge = lx == 0 || lx == Config::chunk_size - 1;
    const bool on_y_edge = ly == 0 || ly == Config::chunk_height - 1;
    const bool on_z_edge = lz == 0 || lz == Config::chunk_size - 1;

    if (on_x_edge) {
        const int nx = cx + (lx == 0 ? -1 : 1);
        if (get_chunk(nx, cy, cz)) {
            enqueue_mesh_rebuild(nx, cy, cz);
        }
    }
    if (on_y_edge) {
        const int ny = cy + (ly == 0 ? -1 : 1);
        if (get_chunk(cx, ny, cz)) {
            enqueue_mesh_rebuild(cx, ny, cz);
        }
    }
    if (on_z_edge) {
        const int nz = cz + (lz == 0 ? -1 : 1);
        if (get_chunk(cx, cy, nz)) {
            enqueue_mesh_rebuild(cx, cy, nz);
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
                // Start above the highest loaded chunk. The loaded world stack is finite
                // from the renderer's point of view, so an unobstructed top cell is a
                // direct sky source.
                uint8_t sky = 15;

                for (int cy = Config::world_max_chunk_y; cy >= Config::world_min_chunk_y; --cy) {
                    Chunk* chunk = get_chunk(cx, cy, cz);
                    if (!chunk) continue;
                    if (!chunk->has_blocks()) continue;

                    for (int ly = Config::chunk_height - 1; ly >= 0; --ly) {
                        const int wy = cy * Config::chunk_height + ly;
                        if (wy < Config::world_min_y || wy > Config::world_max_y) continue;

                        const Block_ID block_id = static_cast<Block_ID>(chunk->get_block(lx, ly, lz));
                        if (!transparent[block_id]) {
                            sky = 0;
                            // Ниже первого непрозрачного блока прямого света
                            // уже быть не может. Не сканируем весь подземный
                            // объём до world_min_y.
                            break;
                        }

                        if (chunk->get_sky_light(lx, ly, lz) != sky) {
                            chunk->set_sky_light(lx, ly, lz, sky);
                            touched_chunks.insert({cx, cy, cz});
                        }
                    }
                    if (sky == 0) {
                        break;
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
}

std::vector<Renderable_Chunk> Chunk_Manager::get_renderable_chunks(
    const Frustum* frustum,
    const glm::vec3* camera_position,
    int flora_render_distance_chunks
) const {
    std::vector<Renderable_Chunk> result;

    m_chunks.for_each([&](const glm::ivec3& key, const std::unique_ptr<Chunk>& chunk) {
        if (chunk->is_empty()) return;
        // Захват области рисует только чанки плана (обычные чанки вокруг игрока, лежащие
        // вне плана, — глубокое подземелье/небо — в кадр не попадают).
        if (capture_active() && !capture_plan_contains(key)) return;

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
            renderable.section_in_frustum[section_index] = in_frustum;
            has_visible_section = has_visible_section || in_frustum;
        }
        if (!has_visible_section) return;

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
    });

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
            m_chunks.for_each([&](const glm::ivec3& key, const std::unique_ptr<Chunk>& chunk) {
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
        m_chunks.for_each([&](const glm::ivec3& key, const std::unique_ptr<Chunk>& chunk) {
            if (chunk->is_mesh_dirty() && capture_plan_contains(key)) ++dirty;
        });
        if (dirty == 0 && m_pending_mesh_rebuilds.empty() && m_mesh_rebuild_requeue.empty()) {
            if (!m_capture.remesh_pass_done) {
                // Второй проход: все меши плана пересобираются уже с окончательным светом
                // (ранние пересборки соседей могли застать неподсчитанный свет).
                m_capture.remesh_pass_done = true;
                m_chunks.for_each([&](const glm::ivec3& key, const std::unique_ptr<Chunk>& chunk) {
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
            m_chunks.for_each([&](const glm::ivec3& key, const std::unique_ptr<Chunk>& chunk) {
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
        std::priority_queue<Chunk_Job, std::vector<Chunk_Job>, Chunk_Job_Priority> empty;
        std::swap(m_pending_jobs, empty);
    }
    m_requested_chunks.clear();

    // Чанки на границе области остались с «обрезанными воздухом» боковыми гранями — пересобрать,
    // если они переживут возврат к обычному окну.
    if (clip.active) {
        m_chunks.for_each([&](const glm::ivec3& key, const std::unique_ptr<Chunk>& chunk) {
            if (key.x == clip.min_cx || key.x == clip.max_cx ||
                key.z == clip.min_cz || key.z == clip.max_cz) {
                chunk->mark_mesh_dirty();
            }
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
