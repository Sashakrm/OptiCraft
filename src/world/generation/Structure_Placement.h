#ifndef OPTICRAFT_STRUCTURE_PLACEMENT_H
#define OPTICRAFT_STRUCTURE_PLACEMENT_H

// Размещение структур (пока только деревни, только позиции).
//
//  * Деревни — «пузыри» по Worley/Voronoi: в каждой ячейке kCellSize x kCellSize один
//    джиттер-центр деревни (хэш от seed и ячейки). Территория деревни — круг своего радиуса
//    вокруг центра; при перекрытии выигрывает ближайший центр (F1). Всё зависит только от
//    мировых координат, порядок загрузки чанков ни на что не влияет.
//  * Центр принимается, если он на суше, местность ровная и в радиусе не проходит река.
//  * Пока деревня — это только маркер: столб kMarkerHeight блоков над центром.
//  * Микрошум (micro_noise) — лёгкая value-noise для структур, которые НЕ привязаны к области
//    (разбросанные объекты). Деревни его не используют.

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>

class River_Network;

struct Village_Site {
    int x = 0;
    int z = 0;
    int radius = 0;   // радиус территории-пузыря, блоков
};

class Structure_Placement {
public:
    using Height_Fn = std::function<int(int world_x, int world_z)>;

    static constexpr int kCellSize = 320;     // сторона ячейки, блоков
    static constexpr int kInset = 64;         // центр не ближе к краю ячейки
    static constexpr int kRadiusMin = 60;     // радиус территории деревни, блоков
    static constexpr int kRadiusMax = 120;
    static constexpr int kMarkerHeight = 100; // высота маркера деревни над землёй

    Structure_Placement(uint32_t seed, Height_Fn height, const River_Network* rivers, int sea_level);
    ~Structure_Placement();

    Structure_Placement(const Structure_Placement&) = delete;
    Structure_Placement& operator=(const Structure_Placement&) = delete;

    // Деревня, центр которой лежит в ячейке (cx, cz), если она там есть.
    std::optional<Village_Site> village_in_cell(int cell_x, int cell_z) const;
    // Лежит ли маркер деревни в этой колонке (центр деревни).
    bool village_center_at(int world_x, int world_z) const;
    // Деревня, чья территория (пузырь) содержит колонку; при перекрытии — ближайший центр (F1).
    std::optional<Village_Site> village_territory_at(int world_x, int world_z) const;

    // Микрошум для структур, не привязанных к области: value-noise в [0, 1], масштаб в блоках.
    static double micro_noise(uint32_t seed, int world_x, int world_z, int scale);

    void clear_cache() const;

private:
    uint64_t m_id;
    uint32_t m_seed;
    int m_sea_level;
    Height_Fn m_height;
    const River_Network* m_rivers;

    mutable std::mutex m_mutex;
    mutable std::unordered_map<int64_t, std::optional<Village_Site>> m_cells;
};

#endif // OPTICRAFT_STRUCTURE_PLACEMENT_H
