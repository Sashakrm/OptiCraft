#ifndef OPTICRAFT_RENDER_WORKER_POOL_H
#define OPTICRAFT_RENDER_WORKER_POOL_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// A small persistent pool for CPU-only frame preparation (0-2 workers). OpenGL calls must stay on
// the context-owning render thread; this pool is deliberately limited to immutable
// input processing (distance calculation, sorting and visibility-list preparation).
class Render_Worker_Pool {
public:
    explicit Render_Worker_Pool(unsigned int requested_workers = 0) {
        if (requested_workers == 0) {
            const unsigned int hw = std::thread::hardware_concurrency();
            // Chunk_Manager owns the bulk of the CPU pool. Add at most two frame workers
            // and only when the machine has enough logical CPUs to spare; the render thread
            // itself still participates in every job.
            requested_workers = hw >= 9 ? 2u : (hw >= 5 ? 1u : 0u);
        }
        requested_workers = std::min(requested_workers, 2u);
        m_workers.reserve(requested_workers);
        for (unsigned int i = 0; i < requested_workers; ++i)
            m_workers.emplace_back([this] { worker_loop(); });
    }

    ~Render_Worker_Pool() { stop(); }
    Render_Worker_Pool(const Render_Worker_Pool&) = delete;
    Render_Worker_Pool& operator=(const Render_Worker_Pool&) = delete;

    unsigned int worker_count() const noexcept { return static_cast<unsigned int>(m_workers.size()); }

    template <typename Function>
    void parallel_for(std::size_t count, std::size_t minimum_parallel_items, Function&& function) {
        if (count == 0) return;
        if (m_workers.empty() || count < minimum_parallel_items) {
            for (std::size_t i = 0; i < count; ++i) function(i);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_task = std::forward<Function>(function);
            m_task_count = count;
            m_next.store(0, std::memory_order_relaxed);
            m_active_workers = m_workers.size();
            ++m_generation;
        }
        m_start_cv.notify_all();

        // The calling/render thread participates. This avoids wasting a core while
        // still keeping every OpenGL call outside this pool.
        run_current_task();

        std::unique_lock<std::mutex> lock(m_mutex);
        m_done_cv.wait(lock, [this] { return m_active_workers == 0; });
        m_task = {};
    }

private:
    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_start_cv;
    std::condition_variable m_done_cv;
    std::function<void(std::size_t)> m_task;
    std::atomic<std::size_t> m_next{0};
    std::size_t m_task_count = 0;
    std::size_t m_active_workers = 0;
    std::size_t m_generation = 0;
    bool m_stopping = false;

    void run_current_task() {
        for (;;) {
            const std::size_t index = m_next.fetch_add(1, std::memory_order_relaxed);
            if (index >= m_task_count) break;
            m_task(index);
        }
    }

    void worker_loop() {
        std::size_t observed_generation = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_start_cv.wait(lock, [this, &observed_generation] {
                    return m_stopping || m_generation != observed_generation;
                });
                if (m_stopping) return;
                observed_generation = m_generation;
            }

            run_current_task();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (--m_active_workers == 0) m_done_cv.notify_one();
            }
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
            ++m_generation;
        }
        m_start_cv.notify_all();
        for (std::thread& worker : m_workers)
            if (worker.joinable()) worker.join();
        m_workers.clear();
    }
};

#endif // OPTICRAFT_RENDER_WORKER_POOL_H
