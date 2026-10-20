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
#include <cstddef>
#include <memory>
#include <vector>

// Width — ширина корзины в единицах приоритета, Count — число корзин (приоритеты >= Width*Count
// попадают в последнюю), Capacity — ячеек кольца в каждой корзине (остальное — резервный список).
template <typename T, int Width = 64, int Count = 256, size_t Capacity = 128>
class Priority_Bucket_Queue {
public:
    static constexpr int kBucketWidth = Width;
    static constexpr int kBucketCount = Count;   // покрывает приоритеты до kBucketWidth*kBucketCount

    Priority_Bucket_Queue() {
        m_buckets.reserve(kBucketCount);
        for (int i = 0; i < kBucketCount; ++i) {
            m_buckets.push_back(std::make_unique<Mpmc_Queue<T>>(Capacity));
        }
    }

    void push(T value, int priority) {
        const int bucket = std::clamp(priority / kBucketWidth, 0, kBucketCount - 1);
        m_buckets[static_cast<size_t>(bucket)]->push(std::move(value));
    }

    bool try_pop(T& out) {
        for (auto& bucket : m_buckets) {
            if (bucket->try_pop(out)) return true;
        }
        return false;
    }

    void clear() {
        for (auto& bucket : m_buckets) bucket->clear();
    }

private:
    std::vector<std::unique_ptr<Mpmc_Queue<T>>> m_buckets;
};

#endif // OPTICRAFT_PRIORITY_BUCKET_QUEUE_H
