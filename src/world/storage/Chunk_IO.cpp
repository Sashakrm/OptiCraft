#include "world/storage/Chunk_IO.h"

#include "utils/logging/Logger.h"

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

void Chunk_IO::release_stage(uint32_t index) {
    uint64_t head = m_free_head.load(std::memory_order_acquire);
    do {
        m_headers[index].next.store(static_cast<uint32_t>(head), std::memory_order_relaxed);
    } while (!m_free_head.compare_exchange_weak(head, pack(tag_of(head) + 1, index),
                                                std::memory_order_release, std::memory_order_acquire));
}

bool Chunk_IO::enqueue_write(const std::shared_ptr<Region_File>& region, const Region_File::Key& key,
                             const Chunk_Block_Grid& blocks) {
    const uint32_t index = acquire_stage();
    if (index == kNone) return false;

    // Буфер принадлежит нам до постановки в очередь, поэтому копию делаем без замка.
    m_stages[index] = blocks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back({region, key, index});
        m_pending[Pending_Key{region.get(), key}] = index;
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
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto it = m_pending.find(Pending_Key{region.get(), keys[i]});
            if (it == m_pending.end()) {
                disk_index.push_back(i);
                continue;
            }
            // Чанк ещё в очереди записи: берём его из буфера очереди, а не с диска (там может быть старая версия).
            const uint32_t copy = acquire_stage();
            if (copy == kNone) continue;   // буферов нет: отдадим генератору
            m_stages[copy] = m_stages[it->second];
            stages[i] = copy;
        }
    }

    // Буферы под чтение с диска.
    std::vector<size_t> to_read;
    std::vector<Region_File::Key> read_keys;
    std::vector<Chunk_Block_Grid*> read_outs;
    for (size_t i : disk_index) {
        const uint32_t s = acquire_stage();
        if (s == kNone) break;   // буферов нет: остальные пойдут генератору
        stages[i] = s;
        to_read.push_back(i);
        read_keys.push_back(keys[i]);
        read_outs.push_back(&m_stages[s]);
    }
    if (read_keys.empty()) return;

    std::vector<uint8_t> ok;
    region->read_batch(read_keys, read_outs, ok);
    for (size_t j = 0; j < to_read.size(); ++j) {
        if (!ok[j]) {
            release_stage(stages[to_read[j]]);
            stages[to_read[j]] = kNone;
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
            if (!batch[indices.front()].region->write_batch(keys, blocks)) {
                LOG_ERROR("Chunk_IO: пачка записи не сохранилась полностью");
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
