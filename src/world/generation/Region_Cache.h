#ifndef OPTICRAFT_REGION_CACHE_H
#define OPTICRAFT_REGION_CACHE_H

// LRU-кэш регионов для генераторов (пещеры, реки), который НЕ держит замок во время построения.
//
// Раньше регион строился под общим мьютексом: один поток считал регион ~0.4 с, а все остальные
// потоки, которым нужен любой другой регион, ждали. Здесь:
//  * первый запросивший ключ создаёт пустое «будущее» и строит регион без замка;
//  * остальные запросившие ТОТ ЖЕ ключ ждут это будущее;
//  * запросы разных регионов строятся параллельно.
// Результат от порядка построения не зависит: регионы детерминированы по мировым координатам.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

template <typename T>
class Region_Cache {
public:
    // build() вызывается без замка и возвращает std::unique_ptr<T> (или исключение).
    template <typename Build>
    std::shared_ptr<const T> get(int64_t key, size_t capacity, Build&& build) {
        std::promise<std::shared_ptr<const T>> promise;
        std::shared_future<std::shared_ptr<const T>> future;
        bool is_builder = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto found = m_entries.find(key);
            if (found != m_entries.end()) {
                m_lru.splice(m_lru.begin(), m_lru, found->second.second);
                future = found->second.first;
            } else {
                future = promise.get_future().share();
                m_lru.push_front(key);
                m_entries.emplace(key, std::make_pair(future, m_lru.begin()));
                if (m_lru.size() > capacity) {
                    // Вытесняем давнего: если он ещё строится, строитель доделает свою работу, а ждущие
                    // уже держат своё будущее.
                    const int64_t victim = m_lru.back();
                    m_lru.pop_back();
                    m_entries.erase(victim);
                }
                is_builder = true;
            }
        }

        if (!is_builder) return future.get();

        try {
            std::shared_ptr<const T> value = build();
            promise.set_value(value);
            return value;
        } catch (...) {
            promise.set_exception(std::current_exception());
            throw;
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lru.clear();
        m_entries.clear();
    }

private:
    using Future = std::shared_future<std::shared_ptr<const T>>;

    std::mutex m_mutex;
    std::list<int64_t> m_lru;   // самый свежий ключ — в начале
    std::unordered_map<int64_t, std::pair<Future, std::list<int64_t>::iterator>> m_entries;
};

#endif // OPTICRAFT_REGION_CACHE_H
