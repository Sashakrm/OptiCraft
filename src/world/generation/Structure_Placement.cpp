#include "world/generation/Structure_Placement.h"

#include "world/generation/River_Network.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {

constexpr size_t kMaxCachedCells = 4096;
constexpr int kRingRadius = 10;      // проверка ровности: центр и 8 точек на радиусе
constexpr int kMaxRelief = 3;        // максимальный перепад высот в проверке, блоков
constexpr double kVillageChance = 0.6;   // шанс, что в ячейке есть деревня

std::atomic<uint64_t> g_next_placement_id{1};

uint64_t splitmix(uint64_t v) {
    v += 0x9E3779B97F4A7C15ULL;
    v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ULL;
    v = (v ^ (v >> 27)) * 0x94D049BB133111EBULL;
    return v ^ (v >> 31);
}

int floordiv(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// Равномерное число в [0, 1) из хэша.
double unit(uint64_t h) {
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

uint64_t hash3(uint32_t seed, int a, int b, uint64_t salt) {
    uint64_t h = splitmix(seed ^ salt);
    h = splitmix(h ^ static_cast<uint64_t>(static_cast<uint32_t>(a)));
    h = splitmix(h ^ static_cast<uint64_t>(static_cast<uint32_t>(b)) << 1);
    return h;
}

inline int64_t cell_key(int cx, int cz) {
    return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cz);
}

struct Last_Cell {
    uint64_t owner = 0;
    int64_t key = 0;
    std::optional<Village_Site> site;
    bool valid = false;
};
thread_local Last_Cell t_last_cell;

} // namespace

Structure_Placement::Structure_Placement(uint32_t seed, Height_Fn height,
                                         const River_Network* rivers, int sea_level)
    : m_id(g_next_placement_id.fetch_add(1)), m_seed(seed), m_sea_level(sea_level),
      m_height(std::move(height)), m_rivers(rivers) {}

Structure_Placement::~Structure_Placement() = default;

void Structure_Placement::clear_cache() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cells.clear();
    t_last_cell.valid = false;
}

std::optional<Village_Site> Structure_Placement::village_in_cell(int cell_x, int cell_z) const {
    const int64_t key = cell_key(cell_x, cell_z);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_cells.find(key);
        if (it != m_cells.end()) return it->second;
    }

    std::optional<Village_Site> result;
    if (unit(hash3(m_seed, cell_x, cell_z, 0xCA11ull)) < kVillageChance) {
        const int span = kCellSize - 2 * kInset;
        const int cx = cell_x * kCellSize + kInset + static_cast<int>(unit(hash3(m_seed, cell_x, cell_z, 0x7001ull)) * span);
        const int cz = cell_z * kCellSize + kInset + static_cast<int>(unit(hash3(m_seed, cell_x, cell_z, 0x7002ull)) * span);
        const int radius = kRadiusMin + static_cast<int>(unit(hash3(m_seed, cell_x, cell_z, 0x7003ull)) * (kRadiusMax - kRadiusMin));

        // Проверка места: суша, ровно, без реки.
        bool ok = true;
        const int centre_h = m_height(cx, cz);
        if (centre_h <= m_sea_level + 2 || centre_h >= 200) ok = false;
        int lo = centre_h, hi = centre_h;
        static constexpr int kRing[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}};
        for (int i = 0; ok && i < 8; ++i) {
            const int x = cx + kRing[i][0] * kRingRadius;
            const int z = cz + kRing[i][1] * kRingRadius;
            const int h = m_height(x, z);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
            if (m_rivers && m_rivers->column_at(x, z).river) ok = false;
        }
        if (ok && hi - lo > kMaxRelief) ok = false;
        if (ok && m_rivers && m_rivers->column_at(cx, cz).river) ok = false;

        if (ok) result = Village_Site{cx, cz, radius};
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_cells.size() >= kMaxCachedCells) m_cells.clear();
    m_cells.emplace(key, result);
    return result;
}

std::optional<Village_Site> Structure_Placement::village_territory_at(int world_x, int world_z) const {
    const int cx = floordiv(world_x, kCellSize);
    const int cz = floordiv(world_z, kCellSize);
    std::optional<Village_Site> best;
    int64_t best_d2 = 0;
    // Центр пузыря может лежать в соседней ячейке, поэтому смотрим 3x3 ячейки.
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const auto site = village_in_cell(cx + dx, cz + dz);
            if (!site) continue;
            const int64_t ex = site->x - world_x, ez = site->z - world_z;
            const int64_t d2 = ex * ex + ez * ez;
            if (d2 > static_cast<int64_t>(site->radius) * site->radius) continue;
            if (!best || d2 < best_d2) {
                best = site;
                best_d2 = d2;
            }
        }
    return best;
}

double Structure_Placement::micro_noise(uint32_t seed, int world_x, int world_z, int scale) {
    const int gx = floordiv(world_x, scale), gz = floordiv(world_z, scale);
    const double fx = static_cast<double>(world_x - gx * scale) / scale;
    const double fz = static_cast<double>(world_z - gz * scale) / scale;
    const double sx = fx * fx * (3 - 2 * fx);
    const double sz = fz * fz * (3 - 2 * fz);
    auto lattice = [&](int i, int j) { return unit(hash3(seed, i, j, 0x51A7ull)); };
    const double top = lattice(gx, gz) * (1 - sx) + lattice(gx + 1, gz) * sx;
    const double bot = lattice(gx, gz + 1) * (1 - sx) + lattice(gx + 1, gz + 1) * sx;
    return top * (1 - sz) + bot * sz;
}

bool Structure_Placement::village_center_at(int world_x, int world_z) const {
    const int cx = floordiv(world_x, kCellSize);
    const int cz = floordiv(world_z, kCellSize);
    const int64_t key = cell_key(cx, cz);
    if (!t_last_cell.valid || t_last_cell.owner != m_id || t_last_cell.key != key) {
        t_last_cell.owner = m_id;
        t_last_cell.key = key;
        t_last_cell.site = village_in_cell(cx, cz);
        t_last_cell.valid = true;
    }
    const auto& site = t_last_cell.site;
    return site && site->x == world_x && site->z == world_z;
}
