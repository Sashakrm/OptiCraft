#ifndef OPTICRAFT_RIVER_NETWORK_H
#define OPTICRAFT_RIVER_NETWORK_H

// Реки и озёра по гидрологии рельефа (вместо шумовых русел).
//
//  * Вода стекает по склону: каждая колонка окна стекает в самого низкого соседа (после сглаживания
//    рельефа); низины заполняются до уровня перелива (priority-flood), поэтому тупиков нет.
//  * Водосбор — число колонок, стекающих через клетку. Где он >= kRiverArea, есть река; где >= kWideArea —
//    она шире. Притоки сливаются, потому что стекание детерминировано по рельефу.
//  * Уровень воды реки чуть ниже рельефа (H - 1), поэтому русло идёт по склону и не режет горы глубоко.
//    Исключение — водопады (уровень резко падает), там русло глубже.
//  * Низина, куда стекает река с водосбором >= kRiverArea, — озеро. Его уровень растёт логарифмически с
//    притоком, но не выше перелива низины. Озеро может стоять выше рельефа (см. Column::lake).
//  * Река доходит до уровня моря или до озера: в её нижней точке она соединяется с океаном или озером.
//
// Ограничение: сток считается в окне региона (регион + kMargin с каждой стороны). Притоки дальше
// kMargin от региона не учитываются, поэтому у границ регионов изредка появляются короткие ручьи без стока.

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
        bool lake = false;      // колонка озера (вода может стоять выше рельефа)
    };

    River_Network(uint32_t seed, Height_Fn height, int sea_level);
    ~River_Network();

    River_Network(const River_Network&) = delete;
    River_Network& operator=(const River_Network&) = delete;

    Column column_at(int world_x, int world_z) const;
    River_Kind kind_at(int world_x, int world_y, int world_z) const;
    void clear_cache() const;
    // Async chunk scheduling API: probe without waiting, then warm on a preparation worker.
    bool region_ready_at(int world_x, int world_z) const;
    void prewarm_region_at(int world_x, int world_z) const;

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
