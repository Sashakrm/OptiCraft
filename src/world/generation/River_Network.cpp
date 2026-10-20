#include "world/generation/River_Network.h"

#include "OpenSimplex2S.h"

#include <algorithm>
#include <atomic>
#include <utility>
#include <cmath>
#include <vector>

namespace {

constexpr int kRegion = River_Network::kRegion;
constexpr int kMargin = River_Network::kMargin;
constexpr int kWin = kRegion + 2 * kMargin;     // сторона окна региона
constexpr int kCoarse = 4;                       // шаг грубой сетки высот
constexpr int kCoarseN = kWin / kCoarse + 1;     // точек грубой сетки на сторону
constexpr int kBasinR = 6;                       // радиус усреднения местности, в клетках грубой сетки (24 блока)
constexpr int kFar = 1000;                       // «далеко от моря» для клеток без моря в окне
constexpr int kCacheRegions = 32;

constexpr double kSlope = 0.12;                  // блоков уровня воды на блок расстояния до моря
constexpr int kFallMin = 3;                      // перепад уровня русла для водопада
constexpr int kFallReach = 4;                    // радиус поиска нижней клетки русла, блоков
constexpr double kWarpFreq = 1.0 / 350.0;
constexpr double kWarpAmp = 60.0;
constexpr double kLineFreq = 1.0 / 420.0;
constexpr double kBaseThreshold = 0.03;         // полуширина русла в единицах |noise|

inline int floordiv(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

inline int64_t region_key(int rx, int rz) {
    return (static_cast<int64_t>(rx) << 32) ^ static_cast<uint32_t>(rz);
}

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

std::shared_ptr<River_Network::Region> River_Network::build_region(int rx, int rz) const {
    auto region = std::make_shared<Region>();
    region->x0 = rx * kRegion;
    region->z0 = rz * kRegion;
    region->cols.assign(static_cast<size_t>(kRegion) * kRegion, Column{});

    auto* env = static_cast<OpenSimplexEnv*>(m_env);
    auto* grad = static_cast<OpenSimplexGradients*>(m_grad);

    const int wx0 = region->x0 - kMargin;
    const int wz0 = region->z0 - kMargin;

    // 1) Грубая сетка высот (шаг 4) и местность вокруг каждой её точки.
    std::vector<double> coarse(static_cast<size_t>(kCoarseN) * kCoarseN);
    for (int j = 0; j < kCoarseN; ++j)
        for (int i = 0; i < kCoarseN; ++i)
            coarse[static_cast<size_t>(j) * kCoarseN + i] =
                m_height(wx0 + i * kCoarse, wz0 + j * kCoarse);

    std::vector<double> basin(coarse.size());
    for (int j = 0; j < kCoarseN; ++j) {
        for (int i = 0; i < kCoarseN; ++i) {
            double sum = 0.0;
            int count = 0;
            for (int dj = -kBasinR; dj <= kBasinR; ++dj) {
                const int jj = j + dj;
                if (jj < 0 || jj >= kCoarseN) continue;
                for (int di = -kBasinR; di <= kBasinR; ++di) {
                    const int ii = i + di;
                    if (ii < 0 || ii >= kCoarseN) continue;
                    sum += coarse[static_cast<size_t>(jj) * kCoarseN + ii];
                    ++count;
                }
            }
            const size_t k = static_cast<size_t>(j) * kCoarseN + i;
            basin[k] = sum / count - coarse[k];   // > 0: мы в низине
        }
    }

    // 2) Полнота окна: высота и местность в каждой клетке окна (билинейная интерполяция).
    std::vector<int> height(static_cast<size_t>(kWin) * kWin);
    std::vector<double> basin_full(height.size());
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
            auto lerp2 = [&](const std::vector<double>& f) {
                const double top = f[a] * (1 - tx) + f[b] * tx;
                const double bot = f[c] * (1 - tx) + f[d] * tx;
                return top * (1 - tz) + bot * tz;
            };
            const size_t idx = static_cast<size_t>(lz) * kWin + lx;
            height[idx] = static_cast<int>(std::floor(lerp2(coarse) + 0.5));
            basin_full[idx] = lerp2(basin);
        }
    }

    // 3) Расстояние до моря внутри окна (BFS по 8 соседям). Море — клетки с высотой <= уровня моря.
    std::vector<int> dist(height.size(), -1);
    std::vector<int> queue;
    queue.reserve(height.size());
    for (size_t k = 0; k < height.size(); ++k) {
        if (height[k] <= m_sea_level) {
            dist[k] = 0;
            queue.push_back(static_cast<int>(k));
        }
    }
    for (size_t head = 0; head < queue.size(); ++head) {
        const int k = queue[head];
        const int lx = k % kWin;
        const int lz = k / kWin;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) continue;
                const int nx = lx + dx, nz = lz + dz;
                if (nx < 0 || nz < 0 || nx >= kWin || nz >= kWin) continue;
                const size_t n = static_cast<size_t>(nz) * kWin + nx;
                if (dist[n] != -1) continue;
                dist[n] = dist[k] + 1;
                queue.push_back(static_cast<int>(n));
            }
        }
    }
    for (auto& d : dist) {
        if (d == -1) d = kFar;
    }

    // 4) Русло: линия нулей шума с искажением координат. Ширина зависит от местности.
    std::vector<uint8_t> channel(height.size(), 0);
    std::vector<int> water(height.size(), 0);
    for (int lz = 0; lz < kWin; ++lz) {
        for (int lx = 0; lx < kWin; ++lx) {
            const size_t idx = static_cast<size_t>(lz) * kWin + lx;
            const int H = height[idx];
            if (H <= m_sea_level) continue;

            const double X = static_cast<double>(wx0 + lx);
            const double Z = static_cast<double>(wz0 + lz);
            const double dx = kWarpAmp * noise2(env, grad, X * kWarpFreq, Z * kWarpFreq + 17.3);
            const double dz = kWarpAmp * noise2(env, grad, X * kWarpFreq + 31.7, Z * kWarpFreq);
            const double v = std::fabs(noise2(env, grad,
                                              (X + dx) * kLineFreq, (Z + dz) * kLineFreq));
            const double widen = std::clamp(basin_full[idx] / 10.0, -0.5, 1.0);
            const double threshold = kBaseThreshold * (1.0 + widen);
            if (v >= threshold) continue;

            channel[idx] = 1;
            // 5) Уровень воды стекает к морю, но не выше земли.
            const double target = m_sea_level + kSlope * dist[idx];
            water[idx] = static_cast<int>(std::floor(std::min(target, static_cast<double>(H))));
        }
    }

    // 6) Водопады: в радиусе kFallReach есть клетка русла ниже уровнем хотя бы на kFallMin блоков.
    // Заполняем только интерьер региона; окрестность всегда внутри окна (kMargin > kFallReach).
    for (int lz = kMargin; lz < kMargin + kRegion; ++lz) {
        for (int lx = kMargin; lx < kMargin + kRegion; ++lx) {
            const size_t idx = static_cast<size_t>(lz) * kWin + lx;
            Column col;
            col.top = height[idx];
            if (channel[idx]) {
                col.river = true;
                col.water = water[idx];
                int lowest = 0;
                bool has_lower = false;
                // Ищем самую низкую клетку русла в радиусе kFallReach: перепад дна = водопад.
                for (int dz = -kFallReach; dz <= kFallReach; ++dz) {
                    for (int dx = -kFallReach; dx <= kFallReach; ++dx) {
                        const size_t n = static_cast<size_t>(lz + dz) * kWin + (lx + dx);
                        if (!channel[n]) continue;
                        if (!has_lower || water[n] < lowest) {
                            lowest = water[n];
                            has_lower = true;
                        }
                    }
                }
                if (has_lower && col.water - lowest >= kFallMin) {
                    col.fall = lowest;
                    col.bed = std::min(col.water, lowest) - 2;
                } else {
                    col.bed = col.water - 2;
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
    if (!col.river || world_y > col.top || world_y <= col.bed) return River_Kind::None;
    return world_y <= col.water ? River_Kind::Water : River_Kind::Air;
}
