#ifndef OPTICRAFT_MPMC_QUEUE_H
#define OPTICRAFT_MPMC_QUEUE_H

// Очередь «много производителей — много потребителей» без замков на горячем пути.
//
// Основа — ограниченное кольцо Вьюкова (Dmitry Vyukov, bounded MPMC queue): каждая ячейка
// несёт счётчик последовательности, и producer/consumer синхронизируются только через CAS
// по позициям головы и хвоста. Если кольцо заполнилось, элемент уходит в резервный список под
// мьютексом: очередь никогда не отбрасывает данные и никогда не блокирует производителя надолго.
//
// Порядок FIFO гарантируется внутри кольца; элементы резервного списка могут обгонять кольцо.
// Для результатов загрузки порядок не важен: результаты проверяются по позиции и поколению.

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <memory>
#include <new>
#include <utility>

template <typename T>
class Mpmc_Queue {
public:
    explicit Mpmc_Queue(size_t capacity = 4096)
        : m_mask(capacity - 1), m_cells(new Cell[capacity]) {
        for (size_t i = 0; i < capacity; ++i) {
            m_cells[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    ~Mpmc_Queue() {
        T item;
        while (try_pop(item)) {}
    }

    Mpmc_Queue(const Mpmc_Queue&) = delete;
    Mpmc_Queue& operator=(const Mpmc_Queue&) = delete;

    // Всегда успешно: при переполнении кольца элемент попадает в резервный список.
    void push(T value) {
        if (try_push(value)) return;
        std::lock_guard<std::mutex> lock(m_overflow_mutex);
        m_overflow.push_back(std::move(value));
        m_overflow_size.fetch_add(1, std::memory_order_release);
    }

    // true и элемент в out — если что-то было; false — очередь пуста.
    bool try_pop(T& out) {
        if (try_pop_ring(out)) return true;
        if (m_overflow_size.load(std::memory_order_acquire) == 0) return false;
        std::lock_guard<std::mutex> lock(m_overflow_mutex);
        if (m_overflow.empty()) return false;
        out = std::move(m_overflow.front());
        m_overflow.pop_front();
        m_overflow_size.fetch_sub(1, std::memory_order_release);
        return true;
    }

    // Выбрасывает всё, что сейчас в очереди (вызывать, когда производители остановлены).
    void clear() {
        T item;
        while (try_pop(item)) {}
    }

private:
    struct Cell {
        std::atomic<size_t> seq{0};
        alignas(T) unsigned char storage[sizeof(T)];
    };

    // Возвращает false, если кольцо полное; value при этом не трогается.
    bool try_push(T& value) {
        size_t pos = m_enqueue.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        for (;;) {
            cell = &m_cells[pos & m_mask];
            const size_t seq = cell->seq.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (m_enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) {
                return false;
            } else {
                pos = m_enqueue.load(std::memory_order_relaxed);
            }
        }
        new (cell->storage) T(std::move(value));
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop_ring(T& out) {
        size_t pos = m_dequeue.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        for (;;) {
            cell = &m_cells[pos & m_mask];
            const size_t seq = cell->seq.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (m_dequeue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) {
                return false;
            } else {
                pos = m_dequeue.load(std::memory_order_relaxed);
            }
        }
        T* item = std::launder(reinterpret_cast<T*>(cell->storage));
        out = std::move(*item);
        item->~T();
        cell->seq.store(pos + m_mask + 1, std::memory_order_release);
        return true;
    }

    const size_t m_mask;
    std::unique_ptr<Cell[]> m_cells;
    alignas(64) std::atomic<size_t> m_enqueue{0};
    alignas(64) std::atomic<size_t> m_dequeue{0};

    std::mutex m_overflow_mutex;
    std::deque<T> m_overflow;
    std::atomic<size_t> m_overflow_size{0};
};

#endif // OPTICRAFT_MPMC_QUEUE_H
