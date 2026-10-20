#ifndef OPTICRAFT_CAVE_NETWORK_H
#define OPTICRAFT_CAVE_NETWORK_H

// Пещеры на путях ("червях") в мировых координатах.
//
//  * Сеть путей живёт от Y = -60 до поверхности. Черви спавнятся в мировых ячейках, у каждого
//    свой seed, симуляция идёт в мировых координатах. Поэтому результат для любой точки мира
//    не зависит ни от порядка загрузки чанков, ни от того, какой регион посчитан первым.
//  * Шахты (часть червей) начинаются у реального верхнего блока рельефа и могут его срезать:
//    так появляются входы с поверхности и в воду (вода — там, где ход касается воды ниже уровня
//    моря).
//  * Ниже -60 обычных пещер нет. Есть закрытые большие пещеры (Y -150..-63), которые не
//    соединяются с сетью. Выше -120 они могут быть затоплены, ниже -120 — всегда сухие.
//
// Данные считаются регионами (256x256 колонок) и кэшируются. Регион сам подтягивает червей из
// ячеек с запасом kMargin, поэтому регионы стыкуются без буферов. Потокобезопасно: один общий
// мьютекс на построение и поиск регионов, дополнительно thread_local кэш последнего региона.

#include "world/generation/Region_Cache.h"

#include <cstdint>
#include <functional>
#include <memory>

enum class Cave_Kind : uint8_t {
    None,            // камень / пещеры нет
    Air,             // сеть путей: воздух
    Water,           // сеть путей, касается воды ниже уровня моря
    Sealed_Dry,      // закрытая большая пещера, сухая
    Sealed_Flooded   // закрытая большая пещера, затопленная (выше -120)
};

class Cave_Network {
public:
    // Y верхнего твёрдого блока в колонке (x, z) в мировых координатах.
    using Height_Fn = std::function<int(int world_x, int world_z)>;

    static constexpr int kNetworkMinY = -60;   // сеть не опускается ниже
    static constexpr int kNetworkMaxY = 130;   // потолок сети (выше рельефа не бывает)
    static constexpr int kSealedMinY = -150;   // нижняя граница закрытых пещер
    static constexpr int kSealedMaxY = -63;    // верхняя граница: два блока камня до сети
    static constexpr int kFloodFloorY = -120;  // ниже этого закрытые пещеры всегда сухие

    Cave_Network(uint32_t seed, Height_Fn height, int sea_level);
    ~Cave_Network();

    Cave_Network(const Cave_Network&) = delete;
    Cave_Network& operator=(const Cave_Network&) = delete;

    // Тип вокселя в мировых координатах. Для Air/Water поверхность не нужна: вход уже вырезан.
    Cave_Kind kind_at(int world_x, int world_y, int world_z) const;

    // Для тестов и инструментов: сбросить кэш регионов (результат не меняется).
    void clear_cache() const;

    struct Region;

private:
    std::shared_ptr<const Region> fetch_region(int rx, int rz) const;

    uint64_t m_id;                 // уникальный id экземпляра для thread_local кэша
    uint32_t m_seed;
    int m_sea_level;
    Height_Fn m_height;

    // Шум: собственный OpenSimplex2S, как и у World_Generator (та же библиотека, свой seed).
    void* m_env = nullptr;         // OpenSimplexEnv*
    void* m_grad = nullptr;        // OpenSimplexGradients*

    mutable Region_Cache<Region> m_regions;   // LRU без замка на построение
};

#endif // OPTICRAFT_CAVE_NETWORK_H
