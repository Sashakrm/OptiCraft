#ifndef OPTICRAFT_PRIORITY_BUCKET_QUEUE_H
#define OPTICRAFT_PRIORITY_BUCKET_QUEUE_H

// Очередь с приоритетом без замков: набор корзин, каждая — свой Mpmc_Queue.
//
// Ниже priority = раньше. Корзина = priority / kBucketWidth (с ограничением сверху). Внутри одной
// корзины порядок FIFO; это потеря точности порядка не больше одной корзины, что для загрузки
// чанков незаметно (корзина ~ четверть шага по кольцу чанков). try_pop идёт от самой низкой корзины.
//
// Вставка и извлечение не берут общих замков: только CAS внутри корзин (и резервный список
// переполненной корзины под её мьютексом).

#include "utils/concurrency/Mpmc_Queue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Width — ширина корзины в единицах приоритета, Count — число корзин (приоритеты >= Width*Count
// попадают в последнюю), Capacity — ячеек кольца в каждой корзине (остальное — резервный список).
template <typename T, int Width = 64, int Count = 256, size_t Capacity = 128>
class Priority_Bucket_Queue {
public:
    static constexpr int kBucketWidth = Width;
    static constexpr int kBucketCount = Count;   // покрывает приоритеты до kBucketWidth*kBucketCount

    static constexpr size_t kFlagWords = (static_cast<size_t>(kBucketCount) + 63u) / 64u;

    Priority_Bucket_Queue() {
        m_buckets.reserve(kBucketCount);
        for (int i = 0; i < kBucketCount; ++i) {
            m_buckets.push_back(std::make_unique<Mpmc_Queue<T>>(Capacity));
        }
        for (auto& flags : m_nonempty) flags.store(0, std::memory_order_relaxed);
    }

    void push(T value, int priority) {
        const int bucket = std::clamp(priority / kBucketWidth, 0, kBucketCount - 1);
        // Публикуем элемент раньше бита непустоты: consumer, увидевший бит, гарантированно
        // попробует уже опубликованный элемент. Несколько producer безопасно ставят один бит.
        m_buckets[static_cast<size_t>(bucket)]->push(std::move(value));
        const size_t word = static_cast<size_t>(bucket) >> 6u;
        const uint64_t mask = uint64_t{1} << (static_cast<unsigned>(bucket) & 63u);
        m_nonempty[word].fetch_or(mask, std::memory_order_release);
    }

    bool try_pop(T& out) {
        // Раньше каждый pop обходил все Count очередей (для мешей их 2176), включая
        // почти всегда пустые. Битовая маска позволяет пропустить пустые корзины блоками
        // по 64; при обычной загрузке проверяются только несколько активных корзин.
        for (size_t word_index = 0; word_index < kFlagWords; ++word_index) {
            uint64_t active = m_nonempty[word_index].load(std::memory_order_acquire);
            while (active != 0) {
                const unsigned bit = std::countr_zero(active);
                const uint64_t mask = uint64_t{1} << bit;
                const size_t bucket_index = word_index * 64u + bit;
                if (bucket_index >= static_cast<size_t>(kBucketCount)) break;

                if (m_buckets[bucket_index]->try_pop(out)) return true;

                // Корзина опустела: снимаем бит и перепроверяем саму очередь. Эта вторая
                // проверка закрывает гонку push/clear: новый элемент не потеряется ни при
                // одном порядке публикации между producer и consumer.
                m_nonempty[word_index].fetch_and(~mask, std::memory_order_acq_rel);
                if (m_buckets[bucket_index]->try_pop(out)) {
                    m_nonempty[word_index].fetch_or(mask, std::memory_order_release);
                    return true;
                }
                active = m_nonempty[word_index].load(std::memory_order_acquire);
            }
        }
        return false;
    }

    void clear() {
        for (auto& bucket : m_buckets) bucket->clear();
        for (auto& flags : m_nonempty) flags.store(0, std::memory_order_release);
    }

private:
    std::vector<std::unique_ptr<Mpmc_Queue<T>>> m_buckets;
    std::array<std::atomic<uint64_t>, kFlagWords> m_nonempty{};
};

#endif // OPTICRAFT_PRIORITY_BUCKET_QUEUE_H
