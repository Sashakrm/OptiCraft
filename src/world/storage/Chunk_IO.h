#ifndef OPTICRAFT_CHUNK_IO_H
#define OPTICRAFT_CHUNK_IO_H

// Буферный слой между чанками и диском.
//
// Зачем: чанк не должен ждать диск. Выгрузка копирует блоки в маленький буфер и сразу возвращает
// управление; писатель в отдельном потоке записывает накопившуюся пачку по регионам, одним захватом
// региона и одним сбросом индекса на пачку. Загрузка тоже пачками: читатель поднимает все чанки
// региона за один проход, в порядке расположения на диске, в буферы; чанки, которые ещё стоят в
// очереди записи, отдаются из буфера очереди, чтобы не читать устаревшую копию с диска.
//
// Буферы — фиксированный пул, выделяется один раз. Список свободных — стек без замков (тот же приём,
// что у Chunk_Slot_Pool: версия в голове защищает от ABA). Очередь записи — один мьютекс, но работает
// пачками, поэтому захватов мало.

#include "world/chunks/Chunk.h"
#include "world/storage/Region_File.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class Chunk_IO {
public:
    static constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

    explicit Chunk_IO(size_t stage_count = 256);
    ~Chunk_IO();

    Chunk_IO(const Chunk_IO&) = delete;
    Chunk_IO& operator=(const Chunk_IO&) = delete;

    // Буфер из пула. kNone — буферы кончились (вызывающий обходит буферы сам).
    uint32_t acquire_stage();
    void release_stage(uint32_t index);
    Chunk_Block_Grid& stage(uint32_t index) { return m_stages[index]; }

    // Копирует блоки в буфер и ставит запись в очередь. false — буферов нет: пишите синхронно.
    bool enqueue_write(const std::shared_ptr<Region_File>& region, const Region_File::Key& key,
                       const Chunk_Block_Grid& blocks);

    // Чанк уже сохранён: лежит в очереди записи или в индексе региона.
    bool has_saved(const std::shared_ptr<Region_File>& region, const Region_File::Key& key);

    // Читает пачку. stages[i] — буфер с блоками чанка keys[i] (его потом надо release_stage)
    // или kNone, если чанка нет и его нужно сгенерировать.
    void read_batch(const std::shared_ptr<Region_File>& region,
                    const std::vector<Region_File::Key>& keys, std::vector<uint32_t>& stages);

    // Ждёт, пока очередь записи полностью уйдёт на диск (нужно при сохранении мира и выходе).
    void drain();

private:
    struct Write_Item {
        std::shared_ptr<Region_File> region;
        Region_File::Key key;
        uint32_t stage = kNone;
    };

    struct Pending_Key {
        const Region_File* region = nullptr;
        Region_File::Key key;
        bool operator==(const Pending_Key& other) const {
            return region == other.region && key == other.key;
        }
    };
    struct Pending_Key_Hash {
        size_t operator()(const Pending_Key& k) const noexcept {
            return std::hash<const void*>{}(k.region) ^ (Region_File::Key_Hash{}(k.key) * 0x9E3779B97F4A7C15ull);
        }
    };

    struct Header {
        std::atomic<uint32_t> next{kNone};
    };

    void writer_loop();
    static uint64_t pack(uint32_t tag, uint32_t index) {
        return (static_cast<uint64_t>(tag) << 32) | index;
    }

    size_t m_stage_count;
    std::unique_ptr<Chunk_Block_Grid[]> m_stages;
    std::unique_ptr<Header[]> m_headers;
    std::atomic<uint64_t> m_free_head{0};

    std::mutex m_mutex;
    std::condition_variable m_queue_cv;
    std::condition_variable m_idle_cv;
    std::vector<Write_Item> m_queue;
    std::unordered_map<Pending_Key, uint32_t, Pending_Key_Hash> m_pending;
    bool m_busy = false;
    bool m_stop = false;
    std::thread m_writer;
};

#endif // OPTICRAFT_CHUNK_IO_H
