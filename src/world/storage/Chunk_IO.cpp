#include "world/storage/Chunk_IO.h"

#include "utils/logging/Logger.h"

#include <algorithm>
#include <map>
#include <utility>

namespace {
uint32_t tag_of(uint64_t head) { return static_cast<uint32_t>(head >> 32); }
} // namespace

Chunk_IO::Chunk_IO(size_t stage_count)
    : m_stage_count(stage_count),
      m_stages(new Chunk_Block_Grid[stage_count]),
      m_headers(new Header[stage_count]) {
    // Свободный список: 0 -> 1 -> ... -> stage_count-1.
    for (size_t i = 0; i < stage_count; ++i) {
        m_headers[i].next.store(i + 1 < stage_count ? static_cast<uint32_t>(i + 1) : kNone,
                                std::memory_order_relaxed);
    }
    m_free_head.store(pack(0, stage_count > 0 ? 0 : kNone), std::memory_order_relaxed);
    m_writer = std::thread(&Chunk_IO::writer_loop, this);
}

Chunk_IO::~Chunk_IO() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_queue_cv.notify_all();
    if (m_writer.joinable()) m_writer.join();   // писатель допишет очередь до конца
}

uint32_t Chunk_IO::acquire_stage() {
    uint64_t head = m_free_head.load(std::memory_order_acquire);
    while (true) {
        const uint32_t index = static_cast<uint32_t>(head);
        if (index == kNone) return kNone;
        const uint32_t next = m_headers[index].next.load(std::memory_order_relaxed);
        if (m_free_head.compare_exchange_weak(head, pack(tag_of(head) + 1, next),
                                              std::memory_order_acquire, std::memory_order_acquire)) {
            return index;
        }
    }
}

uint32_t Chunk_IO::acquire_stage_wait() {
    // Пул обычно не исчерпывается, но при массовой загрузке и сохранении это возможно.
    // В таком случае ждём освобождения буфера вместо того, чтобы объявлять сохранённый чанк
    // отсутствующим и отправлять его на повторную генерацию.
    if (m_stage_count == 0) return kNone;

    // Один и тот же mutex защищает и проверку свободного списка, и переход в wait.
    // Это важно: иначе release_stage мог бы отправить notify между проверкой и wait,
    // после чего читатель уснул бы навсегда, хотя свободный stage уже существует.
    std::unique_lock<std::mutex> lock(m_stage_wait_mutex);
    while (true) {
        const uint32_t index = acquire_stage();
        if (index != kNone) return index;

        m_stage_available_cv.wait(lock, [this] {
            return static_cast<uint32_t>(m_free_head.load(std::memory_order_acquire)) != kNone;
        });
    }
}

void Chunk_IO::release_stage(uint32_t index) {
    {
        // Меняем состояние под тем же mutex, под которым acquire_stage_wait проверяет predicate.
        std::lock_guard<std::mutex> lock(m_stage_wait_mutex);
        uint64_t head = m_free_head.load(std::memory_order_acquire);
        do {
            m_headers[index].next.store(static_cast<uint32_t>(head), std::memory_order_relaxed);
        } while (!m_free_head.compare_exchange_weak(head, pack(tag_of(head) + 1, index),
                                                    std::memory_order_release, std::memory_order_acquire));
    }

    // Разбудить disk_reader, если он ждал свободный stage для чтения сохранённого чанка.
    m_stage_available_cv.notify_all();
}

bool Chunk_IO::enqueue_write(const std::shared_ptr<Region_File>& region, const Region_File::Key& key,
                             const Chunk_Block_Grid& blocks) {
    // Сначала получаем отдельный stage. До постановки в очередь он принадлежит только
    // вызывающему потоку, поэтому исходный чанк можно безопасно скопировать без m_mutex.
    const uint32_t index = acquire_stage();
    if (index == kNone) return false;
    m_stages[index] = blocks;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Pending_Key pending_key{region.get(), key};
        const auto pending = m_pending.find(pending_key);

        if (pending != m_pending.end()) {
            // Если предыдущая версия всё ещё в общей очереди, писатель её ещё не забрал.
            // Заменяем содержимое её stage новой копией вместо накопления десятков устаревших
            // снимков одного чанка при быстром строительстве/работе жидкостей.
            const auto queued = std::find_if(m_queue.begin(), m_queue.end(), [&](const Write_Item& item) {
                return item.region.get() == region.get() && item.key == key &&
                       item.stage == pending->second;
            });
            if (queued != m_queue.end()) {
                m_stages[queued->stage] = blocks;
                release_stage(index);
                return true;
            }
        }

        // Предыдущая версия уже обрабатывается writer-потоком. Добавляем новую запись
        // после неё: так последняя версия чанка гарантированно станет последней на диске.
        m_queue.push_back({region, key, index});
        m_pending[pending_key] = index;
    }
    m_queue_cv.notify_one();
    return true;
}

bool Chunk_IO::has_saved(const std::shared_ptr<Region_File>& region, const Region_File::Key& key) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending.count(Pending_Key{region.get(), key})) return true;
    }
    // Замок очереди уже отпущен: писатель держит замок региона, и вложенных захватов быть не должно.
    return region->contains(key);
}

void Chunk_IO::read_batch(const std::shared_ptr<Region_File>& region,
                          const std::vector<Region_File::Key>& keys, std::vector<uint32_t>& stages) {
    stages.assign(keys.size(), kNone);

    std::vector<size_t> disk_index;
    std::vector<Region_File::Key> read_keys;
    std::vector<Chunk_Block_Grid*> read_outs;
    disk_index.reserve(keys.size());
    read_keys.reserve(keys.size());
    read_outs.reserve(keys.size());

    // Сначала резервируем выходной stage для каждого ключа. Если чанк ещё находится
    // в очереди записи, копируем наиболее свежую версию прямо из её буфера. Если запись
    // уже завершилась, читаем этот же ключ с диска ниже, одним упорядоченным batch-вызовом.
    // Важное отличие от прежней логики: исчерпание пула теперь приводит к ожиданию, а не к
    // возврату kNone, который Chunk_Manager ошибочно трактовал как «чанка нет — генерировать».
    for (size_t i = 0; i < keys.size(); ++i) {
        const uint32_t output_stage = acquire_stage_wait();
        if (output_stage == kNone) continue; // Только для специально созданного пула размера 0.
        stages[i] = output_stage;

        bool copied_from_pending = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_pending.find(Pending_Key{region.get(), keys[i]});
            if (it != m_pending.end()) {
                // Пока удерживается m_mutex, писатель не может удалить запись из pending
                // и освободить её stage; поэтому источник остаётся валидным на всё копирование.
                m_stages[output_stage] = m_stages[it->second];
                copied_from_pending = true;
            }
        }

        if (!copied_from_pending) {
            disk_index.push_back(i);
            read_keys.push_back(keys[i]);
            read_outs.push_back(&m_stages[output_stage]);
        }
    }

    if (read_keys.empty()) return;

    // Region_File сам сортирует ключи по секторам на диске, уменьшая количество seek.
    std::vector<uint8_t> ok;
    region->read_batch(read_keys, read_outs, ok);
    for (size_t j = 0; j < disk_index.size(); ++j) {
        if (!ok[j]) {
            // Реально отсутствующий/повреждённый файл — единственный случай, когда вызывающий
            // код получает kNone и может безопасно перейти к процедурной генерации.
            release_stage(stages[disk_index[j]]);
            stages[disk_index[j]] = kNone;
        }
    }
}

void Chunk_IO::drain() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle_cv.wait(lock, [this] { return m_queue.empty() && !m_busy; });
}

void Chunk_IO::writer_loop() {
    while (true) {
        std::vector<Write_Item> batch;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queue_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) return;   // остановка и очередь пуста
            batch.swap(m_queue);
            m_busy = true;
        }

        // Группируем по регионам: один захват региона и один сброс индекса на пачку.
        std::map<const Region_File*, std::vector<size_t>> groups;
        for (size_t i = 0; i < batch.size(); ++i) groups[batch[i].region.get()].push_back(i);

        for (const auto& [region_ptr, indices] : groups) {
            (void)region_ptr;
            std::vector<Region_File::Key> keys;
            std::vector<const Chunk_Block_Grid*> blocks;
            keys.reserve(indices.size());
            blocks.reserve(indices.size());
            for (size_t i : indices) {
                keys.push_back(batch[i].key);
                blocks.push_back(&m_stages[batch[i].stage]);
            }
            const auto& region = batch[indices.front()].region;
            bool saved = false;

            // Повторяем пакетную запись: временная ошибка файловой системы или неудачный
            // сброс индекса не должны сразу превращать пользовательскую постройку в потерянную.
            for (int attempt = 0; attempt < 3 && !saved; ++attempt) {
                saved = region->write_batch(keys, blocks);
            }

            if (!saved) {
                // Если пакетный сброс всё ещё не проходит, пробуем записать чанки отдельно.
                // Это более медленный аварийный путь, но он позволяет не терять успешные записи
                // из-за одной проблемной записи в общей пачке.
                bool all_saved = true;
                for (size_t j = 0; j < indices.size(); ++j) {
                    bool chunk_saved = false;
                    for (int attempt = 0; attempt < 3 && !chunk_saved; ++attempt) {
                        chunk_saved = region->write(keys[j], *blocks[j]);
                    }
                    all_saved = all_saved && chunk_saved;
                    if (!chunk_saved) {
                        LOG_ERROR("Chunk_IO: не удалось сохранить чанк " +
                                  std::to_string(keys[j].x) + " " +
                                  std::to_string(keys[j].y) + " " +
                                  std::to_string(keys[j].z));
                    }
                }
                if (!all_saved) LOG_ERROR("Chunk_IO: часть чанков не удалось записать даже по отдельности");
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const Write_Item& item : batch) {
                // Запись могла быть перезаписана более свежей с тем же ключом: тогда в карте уже другой буфер.
                const auto it = m_pending.find(Pending_Key{item.region.get(), item.key});
                if (it != m_pending.end() && it->second == item.stage) m_pending.erase(it);
                release_stage(item.stage);
            }
            m_busy = false;
        }
        m_idle_cv.notify_all();
    }
}
