#include "world/generation/River_Network.h"

#include "OpenSimplex2S.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
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

// The priority-flood is a monotone shortest-path style process: every newly pushed
// filled height is max(smooth[cell], popped_height), and can never be below the last
// popped height. A radix heap exploits that property and avoids O(log N) heap work for
// each of ~590k cells. It preserves std::pair<double,int> order exactly: numeric height
// first, cell index second (including treating -0.0 and +0.0 as equal, like double ==).
class Index_Bitset_Min_Queue {
public:
    explicit Index_Bitset_Min_Queue(size_t count)
        : m_words0((count + 63) / 64, 0),
          m_words1((m_words0.size() + 63) / 64, 0),
          m_words2((m_words1.size() + 63) / 64, 0) {}

    bool empty() const { return m_top == 0; }

    void push(int index) {
        const size_t word0 = static_cast<size_t>(index) >> 6;
        const uint64_t bit0 = uint64_t{1} << (static_cast<unsigned>(index) & 63u);
        if (m_words0[word0] & bit0) return; // cells are unique; defensive against duplicates
        const bool was_empty0 = m_words0[word0] == 0;
        m_words0[word0] |= bit0;
        if (!was_empty0) return;

        const size_t word1 = word0 >> 6;
        const uint64_t bit1 = uint64_t{1} << (word0 & 63u);
        const bool was_empty1 = m_words1[word1] == 0;
        m_words1[word1] |= bit1;
        if (!was_empty1) return;

        const size_t word2 = word1 >> 6;
        const uint64_t bit2 = uint64_t{1} << (word1 & 63u);
        const bool was_empty2 = m_words2[word2] == 0;
        m_words2[word2] |= bit2;
        if (was_empty2) m_top |= uint64_t{1} << word2;
    }

    int pop_min() {
        const size_t word2 = static_cast<size_t>(std::countr_zero(m_top));
        const size_t word1 = word2 * 64 + static_cast<size_t>(std::countr_zero(m_words2[word2]));
        const size_t word0 = word1 * 64 + static_cast<size_t>(std::countr_zero(m_words1[word1]));
        const unsigned bit0_index = std::countr_zero(m_words0[word0]);
        const int result = static_cast<int>(word0 * 64 + bit0_index);
        m_words0[word0] &= ~(uint64_t{1} << bit0_index);
        if (m_words0[word0] != 0) return result;

        m_words1[word1] &= ~(uint64_t{1} << (word0 & 63u));
        if (m_words1[word1] != 0) return result;

        m_words2[word2] &= ~(uint64_t{1} << (word1 & 63u));
        if (m_words2[word2] == 0) m_top &= ~(uint64_t{1} << word2);
        return result;
    }

private:
    std::vector<uint64_t> m_words0;
    std::vector<uint64_t> m_words1;
    std::vector<uint64_t> m_words2;
    uint64_t m_top = 0; // current workload has fewer than 64 words at level 2
};

class Monotone_Double_Queue {
public:
    explicit Monotone_Double_Queue(const std::vector<double>& values)
        : m_values(values), m_zero(values.size()) {
        m_buckets.resize(65);
    }

    void set_initial_floor(double value) {
        // Seed cells are inserted before the flood begins and their heights are not
        // monotonic in scan order. Start at their minimum to keep the radix invariant.
        m_last = sortable_key(value);
    }

    void reserve(size_t count) {
        // Buckets are re-used on every redistribution. Reserving the top buckets avoids
        // large reallocations for the bulk of entries, while the zero-key set is bit-packed.
        m_buckets[64].reserve(count / 8);
        m_buckets[63].reserve(count / 8);
    }

    bool empty() const { return m_size == 0; }

    void push(int index) {
        const uint64_t key = sortable_key(m_values[static_cast<size_t>(index)]);
        // With a valid priority flood, keys never go below m_last after seeding. Keep
        // a defensive fallback should a future update violate that invariant.
        if (key < m_last) {
            m_last = key;
            redistribute_all();
        }
        push_with_key(index, key);
        ++m_size;
    }

    int pop_min() {
        if (m_zero.empty()) refill_zero();
        const int result = m_zero.pop_min();
        --m_size;
        return result;
    }

private:
    static uint64_t sortable_key(double value) {
        if (value == 0.0) value = 0.0; // coalesce negative zero to match pair<double,int>
        const uint64_t bits = std::bit_cast<uint64_t>(value);
        return (bits & (uint64_t{1} << 63)) ? ~bits : (bits ^ (uint64_t{1} << 63));
    }

    static unsigned bucket_for(uint64_t diff) {
        return std::bit_width(diff); // 0 for equal keys; 1..64 otherwise
    }

    void push_with_key(int index, uint64_t key) {
        const unsigned bucket = bucket_for(key ^ m_last);
        if (bucket == 0) m_zero.push(index);
        else m_buckets[bucket].push_back(index);
    }

    void refill_zero() {
        size_t bucket = 1;
        while (bucket < m_buckets.size() && m_buckets[bucket].empty()) ++bucket;
        if (bucket == m_buckets.size()) return;

        uint64_t next = UINT64_MAX;
        for (const int index : m_buckets[bucket])
            next = std::min(next, sortable_key(m_values[static_cast<size_t>(index)]));
        m_last = next;

        auto& source = m_buckets[bucket];
        for (const int index : source) {
            const uint64_t key = sortable_key(m_values[static_cast<size_t>(index)]);
            const unsigned target = bucket_for(key ^ m_last);
            if (target == 0) m_zero.push(index);
            else m_buckets[target].push_back(index);
        }
        source.clear();
    }

    void redistribute_all() {
        // Unreachable in normal priority flood. Preserve correctness if a future terrain
        // rule violates monotonicity; this path is not taken by valid terrain inputs.
        std::vector<int> all;
        for (auto& bucket : m_buckets) {
            all.insert(all.end(), bucket.begin(), bucket.end());
            bucket.clear();
        }
        while (!m_zero.empty()) all.push_back(m_zero.pop_min());
        for (const int index : all)
            push_with_key(index, sortable_key(m_values[static_cast<size_t>(index)]));
    }

    const std::vector<double>& m_values;
    Index_Bitset_Min_Queue m_zero;
    std::vector<std::vector<int>> m_buckets;
    uint64_t m_last = 0;
    size_t m_size = 0;
};
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

bool River_Network::region_ready_at(int world_x, int world_z) const {
    const int rx = floordiv(world_x, kRegion);
    const int rz = floordiv(world_z, kRegion);
    const uint64_t packed = (static_cast<uint64_t>(static_cast<uint32_t>(rx)) << 32) |
                            static_cast<uint32_t>(rz);
    return m_regions.is_ready(static_cast<int64_t>(packed));
}

void River_Network::prewarm_region_at(int world_x, int world_z) const {
    (void)fetch_region(floordiv(world_x, kRegion), floordiv(world_z, kRegion));
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
    // Keep exactly the same left-to-right sum order as the original radius-2 box filter,
    // but use a branch-free interior path. More than 99% of samples are interior, where
    // the old inner loop performed five clamps and five index calculations per cell.
    for (int lz = 0; lz < kWin; ++lz) {
        const size_t row = static_cast<size_t>(lz) * kWin;
        for (int lx = 0; lx < kWin; ++lx) {
            double sum = 0.0;
            if (lx >= kSmoothRadius && lx < kWin - kSmoothRadius) {
                sum += height[row + static_cast<size_t>(lx - 2)];
                sum += height[row + static_cast<size_t>(lx - 1)];
                sum += height[row + static_cast<size_t>(lx)];
                sum += height[row + static_cast<size_t>(lx + 1)];
                sum += height[row + static_cast<size_t>(lx + 2)];
            } else {
                for (int d = -kSmoothRadius; d <= kSmoothRadius; ++d) {
                    const int x = std::clamp(lx + d, 0, kWin - 1);
                    sum += height[row + static_cast<size_t>(x)];
                }
            }
            tmp[row + static_cast<size_t>(lx)] = sum / (2 * kSmoothRadius + 1);
        }
    }
    for (int lz = 0; lz < kWin; ++lz) {
        for (int lx = 0; lx < kWin; ++lx) {
            double sum = 0.0;
            if (lz >= kSmoothRadius && lz < kWin - kSmoothRadius) {
                sum += tmp[static_cast<size_t>(lz - 2) * kWin + lx];
                sum += tmp[static_cast<size_t>(lz - 1) * kWin + lx];
                sum += tmp[static_cast<size_t>(lz) * kWin + lx];
                sum += tmp[static_cast<size_t>(lz + 1) * kWin + lx];
                sum += tmp[static_cast<size_t>(lz + 2) * kWin + lx];
            } else {
                for (int d = -kSmoothRadius; d <= kSmoothRadius; ++d) {
                    const int z = std::clamp(lz + d, 0, kWin - 1);
                    sum += tmp[static_cast<size_t>(z) * kWin + lx];
                }
            }
            smooth[static_cast<size_t>(lz) * kWin + lx] = sum / (2 * kSmoothRadius + 1);
        }
    }

    // 3) Заполнение низин. Стоки: море (высота <= уровня моря) и края окна (вода уходит из окна).
    // filled — уровень, до которого поднимается вода в клетке (для не-низин совпадает с рельефом).
    // parent — куда стекает клетка; order — порядок выхода из очереди (потомки после предков).
    std::vector<double> filled(n_cells, 0.0);
    // parent also tracks visitation: -1 = not discovered, -2 = drainage seed/root,
    // >= 0 = parent cell. This removes a separate byte-per-cell array and a memory touch
    // on every neighbour visit while preserving the parent's later accumulation semantics.
    std::vector<int> parent(n_cells, -1);
    std::vector<int> order;
    order.reserve(n_cells);

    Monotone_Double_Queue queue(filled);
    queue.reserve(n_cells);
    // Initialize the radix lower bound to the minimum seed key. Without this pass,
    // negative terrain cells inserted later in scan order would trigger expensive
    // whole-queue redistributions before the priority flood had even started.
    double minimum_seed_height = std::numeric_limits<double>::infinity();
    for (int lz = 0; lz < kWin; ++lz) {
        for (int lx = 0; lx < kWin; ++lx) {
            const size_t k = static_cast<size_t>(lz) * kWin + lx;
            const bool edge = lx == 0 || lz == 0 || lx == kWin - 1 || lz == kWin - 1;
            if (height[k] <= m_sea_level || edge)
                minimum_seed_height = std::min(minimum_seed_height, smooth[k]);
        }
    }
    queue.set_initial_floor(minimum_seed_height);
    for (int lz = 0; lz < kWin; ++lz) {
        for (int lx = 0; lx < kWin; ++lx) {
            const size_t k = static_cast<size_t>(lz) * kWin + lx;
            const bool edge = lx == 0 || lz == 0 || lx == kWin - 1 || lz == kWin - 1;
            if (height[k] <= m_sea_level || edge) {
                parent[k] = -2;
                filled[k] = smooth[k];
                queue.push(static_cast<int>(k));
            }
        }
    }
    // Flat, fixed-order neighbour discovery avoids eight coordinate-pair calculations
    // and a direction loop for every one of ~590k cells. The order is exactly the same as
    // kNeighborDx/kNeighborDz above, which matters because ties are resolved by cell index.
#define OPTICRAFT_DISCOVER_NEIGHBOR(N) do { \
        const size_t ni = static_cast<size_t>(N); \
        if (parent[ni] == -1) { \
            parent[ni] = c; \
            const double ground = smooth[ni]; \
            filled[ni] = ground < spill ? spill : ground; \
            queue.push(static_cast<int>(ni)); \
        } \
    } while (false)
    while (!queue.empty()) {
        const int c = queue.pop_min();
        order.push_back(c);
        const int cx = c % kWin;
        const bool left = cx != 0;
        const bool right = cx != kWin - 1;
        const bool up = c >= kWin;
        const bool down = c < static_cast<int>(n_cells) - kWin;
        const double spill = filled[static_cast<size_t>(c)];
        // Keep the original neighbour order: NW, N, NE, W, E, SW, S, SE.
        if (up) {
            if (left) OPTICRAFT_DISCOVER_NEIGHBOR(c - kWin - 1);
            OPTICRAFT_DISCOVER_NEIGHBOR(c - kWin);
            if (right) OPTICRAFT_DISCOVER_NEIGHBOR(c - kWin + 1);
        }
        if (left) OPTICRAFT_DISCOVER_NEIGHBOR(c - 1);
        if (right) OPTICRAFT_DISCOVER_NEIGHBOR(c + 1);
        if (down) {
            if (left) OPTICRAFT_DISCOVER_NEIGHBOR(c + kWin - 1);
            OPTICRAFT_DISCOVER_NEIGHBOR(c + kWin);
            if (right) OPTICRAFT_DISCOVER_NEIGHBOR(c + kWin + 1);
        }
    }
#undef OPTICRAFT_DISCOVER_NEIGHBOR

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
    };
    std::vector<Basin> basins;
    basins.reserve(n_cells / 64);
    // Reuse one stack for all components. The former implementation allocated a new
    // vector for every basin and stored every component cell twice (stack + cells).
    std::vector<int> stack;
    stack.reserve(n_cells / 8);
    for (size_t start = 0; start < n_cells; ++start) {
        if (basin[start] != -1) continue;
        if (height[start] <= m_sea_level || filled[start] <= smooth[start] + 0.5) continue;
        const int id = static_cast<int>(basins.size());
        basins.push_back(Basin{filled[start], smooth[start], 0});
        Basin& b = basins.back();
        stack.clear();
        stack.push_back(static_cast<int>(start));
        basin[start] = id;
        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
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

    // Equivalent to walking every basin's neighbour list: every non-root cell has one
    // parent, so each external inflow edge can be counted once in a flat linear pass.
    // This avoids millions of neighbour checks and per-basin cell vectors.
    for (size_t n = 0; n < n_cells; ++n) {
        const int p = parent[n];
        if (p < 0) continue;
        const int parent_basin = basin[static_cast<size_t>(p)];
        if (parent_basin >= 0 && basin[n] != parent_basin)
            basins[static_cast<size_t>(parent_basin)].inflow += area[n];
    }

    // Compute each lake's level once, then paint its component in a cache-friendly pass.
    std::vector<double> basin_lake_level(basins.size(), -1e30);
    for (size_t id = 0; id < basins.size(); ++id) {
        const Basin& b = basins[id];
        if (b.inflow < kRiverArea) continue;
        const double doublings = std::log2(static_cast<double>(b.inflow) / kRiverArea);
        const double depth = std::min(b.spill - b.bottom, kLakeDepthPerDoubling * (1.0 + doublings));
        basin_lake_level[id] = b.bottom + depth;
    }
    std::vector<uint8_t> lake(n_cells, 0);
    std::vector<int> lake_level(n_cells, 0);
    for (size_t k = 0; k < n_cells; ++k) {
        const int id = basin[k];
        if (id < 0) continue;
        const double level = basin_lake_level[static_cast<size_t>(id)];
        if (level < -1e20 || smooth[k] >= level) continue;
        lake[k] = 1;
        lake_level[k] = static_cast<int>(std::floor(level));
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
