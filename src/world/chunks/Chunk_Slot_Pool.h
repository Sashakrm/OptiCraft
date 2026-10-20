#ifndef OPTICRAFT_CHUNK_SLOT_POOL_H
#define OPTICRAFT_CHUNK_SLOT_POOL_H

// Фиксированный пул слотов под объекты Chunk.
//
// Идея перенесена из пула KeruCat/Voxels (Engine/Memory), но упрощена под чанки:
//  * Все слоты одного размера, поэтому дефрагментации и списка узлов нет: свободный слот — это
//    просто элемент списка свободных индексов.
//  * Список свободных — стек Трайбера (Treiber stack) без замков. Голова хранит пару
//    (счётчик версии, индекс): счётчик не даёт ABA, когда слот освободили и взяли заново между
//    чтением головы и CAS.
//  * У каждого слота есть поколение: оно растёт при каждом освобождении. Поколение можно
//    сравнить с сохранённым, чтобы понять, что объект уже другой.
//  * Память под слоты выделяется одним куском без инициализации: страницы коммитятся, только
//    когда слот реально используется.
//  * Переполнение пула не аллоцирует новую память: acquire() возвращает nullptr, и вызывающий
//    код откладывает загрузку.
//
// Владение: Chunk_Ptr — это unique_ptr с делетером, который возвращает слот в пул. Поэтому
// существующий код, который держал std::unique_ptr<Chunk>, меняется только в типе.
// Освобождать чанк можно из любого потока; создавать — тоже из любого (lock-free).

#include "world/chunks/Chunk.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

class Chunk_Slot_Pool {
public:
    explicit Chunk_Slot_Pool(size_t capacity)
        : m_capacity(capacity),
          m_stride(round_up(sizeof(Chunk), alignof(Chunk))),
          m_headers(new Header[capacity]) {
        m_storage = static_cast<std::byte*>(
            ::operator new(capacity * m_stride, std::align_val_t(alignof(Chunk))));
        // Список свободных: 0 -> 1 -> ... -> capacity-1 -> конец.
        for (size_t i = 0; i < capacity; ++i) {
            m_headers[i].next.store(i + 1 < capacity ? static_cast<uint32_t>(i + 1) : kNone,
                                    std::memory_order_relaxed);
        }
        m_head.store(pack(0, capacity > 0 ? 0 : kNone), std::memory_order_relaxed);
    }

    ~Chunk_Slot_Pool() {
        // К моменту уничтожения все чанки должны быть освобождены (их держит Chunk_Manager, который
        // объявлен позже пула и уничтожается раньше).
        ::operator delete(m_storage, std::align_val_t(alignof(Chunk)));
    }

    Chunk_Slot_Pool(const Chunk_Slot_Pool&) = delete;
    Chunk_Slot_Pool& operator=(const Chunk_Slot_Pool&) = delete;

    // Берёт свободный слот и строит в нём Chunk. nullptr — пул исчерпан (память не трогается).
    template <typename... Args>
    Chunk* acquire(Args&&... args) {
        uint64_t head = m_head.load(std::memory_order_acquire);
        uint32_t index = kNone;
        while (true) {
            index = static_cast<uint32_t>(head);
            if (index == kNone) return nullptr;
            const uint32_t next = m_headers[index].next.load(std::memory_order_relaxed);
            const uint64_t desired = pack(tag_of(head) + 1, next);
            if (m_head.compare_exchange_weak(head, desired, std::memory_order_acquire,
                                             std::memory_order_acquire)) {
                break;
            }
        }

        Chunk* chunk = nullptr;
        try {
            chunk = new (slot_memory(index)) Chunk(std::forward<Args>(args)...);
        } catch (...) {
            push_free(index);
            throw;
        }
        m_in_use.fetch_add(1, std::memory_order_relaxed);
        return chunk;
    }

    // Возвращает слот в пул. Вызывается из делетера Chunk_Ptr.
    void release(Chunk* chunk) noexcept {
        const uint32_t index = index_of(chunk);
        chunk->~Chunk();
        m_headers[index].generation.fetch_add(1, std::memory_order_relaxed);
        m_in_use.fetch_sub(1, std::memory_order_relaxed);
        push_free(index);
    }

    // Поколение слота: меняется при каждом освобождении. Сохранённое значение позволяет понять,
    // что на этом месте уже другой объект.
    uint32_t generation_of(const Chunk* chunk) const {
        return m_headers[index_of(chunk)].generation.load(std::memory_order_relaxed);
    }

    size_t capacity() const { return m_capacity; }
    size_t in_use() const { return m_in_use.load(std::memory_order_relaxed); }

private:
    static constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

    struct Header {
        std::atomic<uint32_t> generation{0};
        std::atomic<uint32_t> next{kNone};
    };

    static constexpr size_t round_up(size_t value, size_t align) {
        return (value + align - 1) / align * align;
    }
    static constexpr uint64_t pack(uint32_t tag, uint32_t index) {
        return (static_cast<uint64_t>(tag) << 32) | index;
    }
    static constexpr uint32_t tag_of(uint64_t head) { return static_cast<uint32_t>(head >> 32); }

    std::byte* slot_memory(uint32_t index) const { return m_storage + static_cast<size_t>(index) * m_stride; }

    uint32_t index_of(const Chunk* chunk) const {
        const size_t offset = static_cast<size_t>(reinterpret_cast<const std::byte*>(chunk) - m_storage);
        return static_cast<uint32_t>(offset / m_stride);
    }

    void push_free(uint32_t index) noexcept {
        uint64_t head = m_head.load(std::memory_order_acquire);
        do {
            m_headers[index].next.store(static_cast<uint32_t>(head), std::memory_order_relaxed);
        } while (!m_head.compare_exchange_weak(head, pack(tag_of(head) + 1, index),
                                               std::memory_order_release, std::memory_order_acquire));
    }

    size_t m_capacity;
    size_t m_stride;
    std::byte* m_storage = nullptr;
    std::unique_ptr<Header[]> m_headers;
    std::atomic<uint64_t> m_head{0};
    std::atomic<size_t> m_in_use{0};
};

// Делетер для Chunk_Ptr: возвращает слот пулу. pool == nullptr допустим только для пустого указателя.
struct Chunk_Slot_Release {
    Chunk_Slot_Pool* pool = nullptr;
    void operator()(Chunk* chunk) const noexcept {
        if (chunk) pool->release(chunk);
    }
};

using Chunk_Ptr = std::unique_ptr<Chunk, Chunk_Slot_Release>;

#endif // OPTICRAFT_CHUNK_SLOT_POOL_H
