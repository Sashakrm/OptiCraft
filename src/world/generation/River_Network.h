#ifndef OPTICRAFT_RIVER_NETWORK_H
#define OPTICRAFT_RIVER_NETWORK_H

// Реки: шумовые русла с гидрологическими элементами (гибрид варианта 1 и 2).
//
//  * Русло — линия нулей |noise| на 2D-шуме с domain warping: извилистая, детерминированная
//    по мировым координатам.
//  * Ширина растёт там, где колонка ниже окружающей местности (плёсы и озёрца в низинах) и
//    сужается на склонах.
//  * Уровень воды стекает к морю: T = sea + kSlope * расстояние_до_моря, но не выше земли.
//    Так вода никогда не висит над ложбиной, а у устья совпадает с уровнем моря.
//  * Водопад — там, где в радиусе 4 блоков есть клетка русла с уровнем воды ниже хотя бы на 3 блока.
//
// Ограничение (вариант А, «окно»): расстояние до моря считается только внутри окна региона
// (регион + kMargin с каждой стороны). У очень длинных рек уровень может давать небольшие
// ступеньки на границах окон. Результат детерминирован для колонки при фиксированном регионе.

#include "world/generation/Region_Cache.h"

#include <cstdint>
#include <functional>
#include <memory>

enum class River_Kind : uint8_t {
    None,    // не река
    Air,     // вырезано русло над водой
    Water    // вода
};

class River_Network {
public:
    // Y верхнего твёрдого блока в колонке (x, z).
    using Height_Fn = std::function<int(int world_x, int world_z)>;

    static constexpr int kRegion = 256;   // сторона региона, колонок
    static constexpr int kMargin = 256;   // запас окна вокруг региона
    static constexpr int kNoFall = INT32_MIN;

    struct Column {
        bool river = false;
        int top = 0;            // H: верхний блок рельефа
        int water = 0;          // W: уровень воды
        int bed = 0;            // дно русла (вода выше этого Y)
        int fall = kNoFall;     // нижний уровень водопада в этой колонке, если есть
    };

    River_Network(uint32_t seed, Height_Fn height, int sea_level);
    ~River_Network();

    River_Network(const River_Network&) = delete;
    River_Network& operator=(const River_Network&) = delete;

    Column column_at(int world_x, int world_z) const;
    River_Kind kind_at(int world_x, int world_y, int world_z) const;
    void clear_cache() const;

    struct Region;

private:
    std::shared_ptr<const Region> fetch_region(int rx, int rz) const;
    std::shared_ptr<Region> build_region(int rx, int rz) const;

    uint32_t m_seed;
    int m_sea_level;
    Height_Fn m_height;
    void* m_env = nullptr;     // OpenSimplexEnv*
    void* m_grad = nullptr;    // OpenSimplexGradients*

    uint64_t m_id;             // уникальный id экземпляра для thread_local кэша
    mutable Region_Cache<Region> m_regions;   // LRU без замка на построение
};

#endif // OPTICRAFT_RIVER_NETWORK_H
