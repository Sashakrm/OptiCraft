#include "world/generation/River_Network.h"

#include "OpenSimplex2S.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <queue>
#include <utility>
#include <vector>

namespace {

constexpr int kRegion = River_Network::kRegion;
constexpr int kMargin = River_Network::kMargin;
constexpr int kWin = kRegion + 2 * kMargin;     // сторона окна региона, колонок
constexpr int kCoarse = 4;                       // шаг грубой сетки высот
constexpr int kCoarseN = kWin / kCoarse + 1;     // точек грубой сетки на сторону
constexpr int kCacheRegions = 32;

// Сглаживание рельефа перед стеканием воды: убирает микровпадины, на которых вода застревала бы.
constexpr int kSmoothRadius = 2;

// Водосбор (число колонок, стекающих через клетку), с которого начинается река.
constexpr int kRiverArea = 400;
// С этого водосбора река становится шире (захватывает соседей).
constexpr int kWideArea = 1600;
// Глубина озера растёт логарифмически от притока: на каждое удвоение притока (сверх kRiverArea).
constexpr double kLakeDepthPerDoubling = 1.5;

constexpr int kFallMin = 3;                      // перепад уровня воды для водопада
constexpr int kFallReach = 4;                    // радиус поиска нижней клетки русла

inline int floordiv(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

inline int64_t region_key(int rx, int rz) {
    return (static_cast<int64_t>(rx) << 32) ^ static_cast<uint32_t>(rz);
}

constexpr int kNeighborDx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr int kNeighborDz[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

} // namespace

struct River_Network::Region {
    int x0 = 0;   // мировой X левого края региона
    int z0 = 0;   // мировой Z верхнего края региона
    std::vector<Column> cols;   // kRegion * kRegion, строка за строкой по Z
};

namespace {
std::atomic<uint64_t> g_next_river_id{1};

// Последний использованный регион этого потока: соседние колонки почти всегда в одном регионе.
struct Last_Region {
    uint64_t owner = 0;
    int64_t key = 0;
    std::shared_ptr<const River_Network::Region> region;
};
thread_local Last_Region t_last_region;
} // namespace

River_Network::River_Network(uint32_t seed, Height_Fn height, int sea_level)
    : m_seed(seed), m_sea_level(sea_level), m_height(std::move(height)), m_id(g_next_river_id.fetch_add(1)) {
    m_env = initOpenSimplex();
    m_grad = newOpenSimplexGradients(static_cast<OpenSimplexEnv*>(m_env), static_cast<long>(seed ^ 0x5157u));
}

River_Network::~River_Network() = default;

void River_Network::clear_cache() const {
    m_regions.clear();
}

std::shared_ptr<const River_Network::Region> River_Network::fetch_region(int rx, int rz) const {
    // Построение идёт без общего замка: см. Region_Cache.
    return m_regions.get(region_key(rx, rz), static_cast<size_t>(kCacheRegions),
                         [&]() { return build_region(rx, rz); });
}

// Гидрология окна региона:
//  1) рельеф (как раньше: грубая сетка + билинейная интерполяция);
//  2) сглаживание рельефа;
//  3) заполнение низин (priority-flood): каждая клетка получает «родителя» — соседа, куда стекает вода;
//     стоки — море и края окна;
//  4) водосбор: сколько колонок стекает через клетку;
//  5) низины, куда стекают реки, заполняются водой до уровня, который растёт с притоком — это озёра;
//  6) реки — клетки с большим водосбором; их уровень воды чуть ниже рельефа, поэтому русло идёт
//     по склону и не режет горы глубже нескольких блоков.
std::shared_ptr<River_Network::Region> River_Network::build_region(int rx, int rz) const {
    auto region = std::make_shared<Region>();
    region->x0 = rx * kRegion;
    region->z0 = rz * kRegion;
    region->cols.assign(static_cast<size_t>(kRegion) * kRegion, Column{});

    const int wx0 = region->x0 - kMargin;
    const int wz0 = region->z0 - kMargin;
    const size_t n_cells = static_cast<size_t>(kWin) * kWin;

    // 1) Грубая сетка высот и билинейная интерполяция в каждую колонку окна.
    std::vector<double> coarse(static_cast<size_t>(kCoarseN) * kCoarseN);
    for (int j = 0; j < kCoarseN; ++j)
        for (int i = 0; i < kCoarseN; ++i)
            coarse[static_cast<size_t>(j) * kCoarseN + i] =
                m_height(wx0 + i * kCoarse, wz0 + j * kCoarse);

    std::vector<int> height(n_cells);
    for (int lz = 0; lz < kWin; ++lz) {
        const int j0 = std::min(lz / kCoarse, kCoarseN - 2);
        const double tz = static_cast<double>(lz) / kCoarse - j0;
        for (int lx = 0; lx < kWin; ++lx) {
            const int i0 = std::min(lx / kCoarse, kCoarseN - 2);
            const double tx = static_cast<double>(lx) / kCoarse - i0;
            const size_t a = static_cast<size_t>(j0) * kCoarseN + i0;
            const size_t b = a + 1;
            const size_t c = a + kCoarseN;
            const size_t d = c + 1;
            const double top = coarse[a] * (1 - tx) + coarse[b] * tx;
            const double bot = coarse[c] * (1 - tx) + coarse[d] * tx;
            height[static_cast<size_t>(lz) * kWin + lx] =
                static_cast<int>(std::floor(top * (1 - tz) + bot * tz + 0.5));
        }
    }

    // 2) Сглаживание рельефа: box-фильтр, отдельно по X и по Z (разделимый).
    std::vector<double> tmp(n_cells);
    std::vector<double> smooth(n_cells);
    for (int lz = 0; lz < kWin; ++lz) {
        for (int lx = 0; lx < kWin; ++lx) {
            double sum = 0.0;
            for (int d = -kSmoothRadius; d <= kSmoothRadius; ++d) {
                const int x = std::clamp(lx + d, 0, kWin - 1);
                sum += height[static_cast<size_t>(lz) * kWin + x];
            }
            tmp[static_cast<size_t>(lz) * kWin + lx] = sum / (2 * kSmoothRadius + 1);
        }
    }
    for (int lz = 0; lz < kWin; ++lz) {
        for (int lx = 0; lx < kWin; ++lx) {
            double sum = 0.0;
            for (int d = -kSmoothRadius; d <= kSmoothRadius; ++d) {
                const int z = std::clamp(lz + d, 0, kWin - 1);
                sum += tmp[static_cast<size_t>(z) * kWin + lx];
            }
            smooth[static_cast<size_t>(lz) * kWin + lx] = sum / (2 * kSmoothRadius + 1);
        }
    }

    // 3) Заполнение низин. Стоки: море (высота <= уровня моря) и края окна (вода уходит из окна).
    // filled — уровень, до которого поднимается вода в клетке (для не-низин совпадает с рельефом).
    // parent — куда стекает клетка; order — порядок выхода из очереди (потомки после предков).
    std::vector<double> filled(n_cells, 0.0);
    std::vector<int> parent(n_cells, -1);
    std::vector<uint8_t> visited(n_cells, 0);
    std::vector<int> order;
    order.reserve(n_cells);

    using Entry = std::pair<double, int>;   // (filled, index)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
    for (int lz = 0; lz < kWin; ++lz) {
        for (int lx = 0; lx < kWin; ++lx) {
            const size_t k = static_cast<size_t>(lz) * kWin + lx;
            const bool edge = lx == 0 || lz == 0 || lx == kWin - 1 || lz == kWin - 1;
            if (height[k] <= m_sea_level || edge) {
                visited[k] = 1;
                filled[k] = smooth[k];
                queue.emplace(filled[k], static_cast<int>(k));
            }
        }
    }
    while (!queue.empty()) {
        const int c = queue.top().second;
        queue.pop();
        order.push_back(c);
        const int cx = c % kWin;
        const int cz = c / kWin;
        for (int dir = 0; dir < 8; ++dir) {
            const int nx = cx + kNeighborDx[dir];
            const int nz = cz + kNeighborDz[dir];
            if (nx < 0 || nz < 0 || nx >= kWin || nz >= kWin) continue;
            const size_t n = static_cast<size_t>(nz) * kWin + nx;
            if (visited[n]) continue;
            visited[n] = 1;
            parent[n] = c;
            filled[n] = std::max(smooth[n], filled[c]);
            queue.emplace(filled[n], static_cast<int>(n));
        }
    }

    // 4) Водосбор: сколько клеток стекает через каждую (сама клетка тоже). Потомки обрабатываются раньше
    // предков: порядок обратный выходу из очереди.
    std::vector<int> area(n_cells, 1);
    for (size_t i = order.size(); i-- > 0;) {
        const int k = order[i];
        if (parent[k] >= 0) area[parent[k]] += area[k];
    }

    // 5) Низины: клетки, где вода поднята выше рельефа (filled > smooth). Связные области с одинаковым
    // уровнем — это одна низина; её уровень заполнения (spill) — общий уровень области.
    std::vector<int> basin(n_cells, -1);
    struct Basin {
        double spill = 0.0;
        double bottom = 1e9;
        long inflow = 0;
        std::vector<int> cells;
    };
    std::vector<Basin> basins;
    for (size_t start = 0; start < n_cells; ++start) {
        if (basin[start] != -1) continue;
        if (height[start] <= m_sea_level || filled[start] <= smooth[start] + 0.5) continue;
        const int id = static_cast<int>(basins.size());
        basins.emplace_back();
        Basin& b = basins.back();
        b.spill = filled[start];
        std::vector<int> stack{static_cast<int>(start)};
        basin[start] = id;
        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
            b.cells.push_back(c);
            b.bottom = std::min(b.bottom, smooth[c]);
            const int cx = c % kWin;
            const int cz = c / kWin;
            for (int dir = 0; dir < 8; ++dir) {
                const int nx = cx + kNeighborDx[dir];
                const int nz = cz + kNeighborDz[dir];
                if (nx < 0 || nz < 0 || nx >= kWin || nz >= kWin) continue;
                const size_t n = static_cast<size_t>(nz) * kWin + nx;
                if (basin[n] != -1) continue;
                if (height[n] <= m_sea_level || filled[n] <= smooth[n] + 0.5) continue;
                if (std::fabs(filled[n] - b.spill) > 1e-9) continue;
                basin[n] = id;
                stack.push_back(static_cast<int>(n));
            }
        }
    }

    // Приток в низину: водосборы соседних клеток вне низины, которые стекают в неё.
    for (size_t id = 0; id < basins.size(); ++id) {
        Basin& b = basins[id];
        for (const int c : b.cells) {
            const int cx = c % kWin;
            const int cz = c / kWin;
            for (int dir = 0; dir < 8; ++dir) {
                const int nx = cx + kNeighborDx[dir];
                const int nz = cz + kNeighborDz[dir];
                if (nx < 0 || nz < 0 || nx >= kWin || nz >= kWin) continue;
                const size_t n = static_cast<size_t>(nz) * kWin + nx;
                if (basin[n] == static_cast<int>(id)) continue;
                if (parent[n] == c) b.inflow += area[n];
            }
        }
    }

    // Уровень озера: глубина растёт с притоком. Без притока достаточной реки озера нет.
    std::vector<uint8_t> lake(n_cells, 0);
    std::vector<int> lake_level(n_cells, 0);
    for (const Basin& b : basins) {
        if (b.inflow < kRiverArea) continue;
        const double doublings = std::log2(static_cast<double>(b.inflow) / kRiverArea);
        const double depth = std::min(b.spill - b.bottom, kLakeDepthPerDoubling * (1.0 + doublings));
        const double level = b.bottom + depth;
        for (const int c : b.cells) {
            if (smooth[c] < level) {
                lake[c] = 1;
                lake_level[c] = static_cast<int>(std::floor(level));
            }
        }
    }

    // 6) Реки: клетки с достаточным водосбором вне озёр. Вода — чуть ниже рельефа, поэтому русло
    // стекает по склону. Вода не поднимается выше рельефа, ниже уровня моря не опускается.
    std::vector<uint8_t> river(n_cells, 0);
    for (size_t k = 0; k < n_cells; ++k) {
        if (height[k] <= m_sea_level || lake[k]) continue;
        if (area[k] < kRiverArea) continue;
        river[k] = 1;
        if (area[k] >= kWideArea) {
            const int cx = static_cast<int>(k) % kWin;
            const int cz = static_cast<int>(k) / kWin;
            for (int dir = 0; dir < 8; ++dir) {
                const int nx = cx + kNeighborDx[dir];
                const int nz = cz + kNeighborDz[dir];
                if (nx < 0 || nz < 0 || nx >= kWin || nz >= kWin) continue;
                const size_t n = static_cast<size_t>(nz) * kWin + nx;
                if (height[n] > m_sea_level && !lake[n]) river[n] = 1;
            }
        }
    }

    std::vector<int> water(n_cells, 0);
    for (size_t k = 0; k < n_cells; ++k) {
        if (lake[k]) {
            water[k] = lake_level[k];
        } else if (river[k]) {
            water[k] = std::max(height[k] - 1, m_sea_level);
        }
    }

    // 7) Колонки региона (интерьер), водопады там, где уровень воды резко падает.
    for (int lz = kMargin; lz < kMargin + kRegion; ++lz) {
        for (int lx = kMargin; lx < kMargin + kRegion; ++lx) {
            const size_t idx = static_cast<size_t>(lz) * kWin + lx;
            Column col;
            col.top = height[idx];
            if (river[idx] || lake[idx]) {
                col.river = true;
                col.lake = lake[idx] != 0;
                col.water = water[idx];
                col.bed = std::min(col.top, col.water) - 2;
                if (!col.lake) {
                    int lowest = col.water;
                    for (int dz = -kFallReach; dz <= kFallReach; ++dz) {
                        for (int dx = -kFallReach; dx <= kFallReach; ++dx) {
                            const size_t n = static_cast<size_t>(lz + dz) * kWin + (lx + dx);
                            if (!river[n]) continue;
                            lowest = std::min(lowest, water[n]);
                        }
                    }
                    if (col.water - lowest >= kFallMin) {
                        col.fall = lowest;
                        col.bed = lowest - 2;
                    }
                }
            }
            region->cols[static_cast<size_t>(lz - kMargin) * kRegion + (lx - kMargin)] = col;
        }
    }

    return region;
}

River_Network::Column River_Network::column_at(int world_x, int world_z) const {
    const int rx = floordiv(world_x, kRegion);
    const int rz = floordiv(world_z, kRegion);
    const int64_t key = region_key(rx, rz);
    if (t_last_region.owner != m_id || t_last_region.key != key || !t_last_region.region) {
        t_last_region.owner = m_id;
        t_last_region.key = key;
        t_last_region.region = fetch_region(rx, rz);
    }
    const auto& region = t_last_region.region;
    const int lx = world_x - region->x0;
    const int lz = world_z - region->z0;
    return region->cols[static_cast<size_t>(lz) * kRegion + lx];
}

River_Kind River_Network::kind_at(int world_x, int world_y, int world_z) const {
    const Column col = column_at(world_x, world_z);
    if (!col.river || world_y <= col.bed) return River_Kind::None;
    if (world_y <= col.water) return River_Kind::Water;   // вода в том числе над рельефом (озёра)
    if (world_y > col.top) return River_Kind::None;
    return River_Kind::Air;
}
