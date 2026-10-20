#include "world/generation/Cave_Network.h"

#include "OpenSimplex2S.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>
#include <vector>

namespace {

// ---- Регионы и сетка ----------------------------------------------------------------------
constexpr int kRegion = 256;          // сторона региона, колонок
constexpr int kMargin = 320;          // запас спавна червей: больше максимального охвата червя
constexpr int kCell = 64;             // сторона ячейки спавна червей
constexpr int kCacheRegions = 24;     // сколько регионов держим в памяти

constexpr int kNetNY = Cave_Network::kNetworkMaxY - Cave_Network::kNetworkMinY + 1;
constexpr int kSealedNY = Cave_Network::kSealedMaxY - Cave_Network::kSealedMinY + 1;

// ---- Черви --------------------------------------------------------------------------------
constexpr int kRoof = 5;              // крыша над обычным ходом
constexpr int kShaftLen = 14;         // первые шаги шахты могут срезать верхний блок
constexpr double kEntranceChance = 0.9;   // доля червей, начинающих шахтой у поверхности
constexpr double kWormsBase = 1.0;
constexpr double kWormsExtra = 1.0;
constexpr double kChamberChance = 0.012;  // зал на шаг после 20 шагов
constexpr double kBranchChance = 0.008;   // ответвление на шаг

// ---- Закрытые пещеры ----------------------------------------------------------------------
constexpr int kSealedCell = 96;
constexpr int kSealedMargin = 24;     // центры камер до 20 блоков от края ячейки

uint64_t splitmix(uint64_t v) {
    v += 0x9E3779B97F4A7C15ULL;
    v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ULL;
    v = (v ^ (v >> 27)) * 0x94D049BB133111EBULL;
    return v ^ (v >> 31);
}

int floordiv(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() { return s = splitmix(s); }
    double uni() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double range(double a, double b) { return a + (b - a) * uni(); }
};

inline size_t net_index(int lx, int y, int lz) {
    return (static_cast<size_t>(y - Cave_Network::kNetworkMinY) * kRegion + lz) * kRegion + lx;
}
inline size_t sealed_index(int lx, int y, int lz) {
    return (static_cast<size_t>(y - Cave_Network::kSealedMinY) * kRegion + lz) * kRegion + lx;
}
inline void set_bit(std::vector<uint64_t>& v, size_t i) { v[i >> 6] |= (1ull << (i & 63)); }
inline bool get_bit(const std::vector<uint64_t>& v, size_t i) { return (v[i >> 6] >> (i & 63)) & 1ull; }

std::atomic<uint64_t> g_next_id{1};

}  // namespace

// Регион хранит только биты. Поля совпадают по индексу: net/water — сеть, sealed/flood — закрытые.
struct Cave_Network::Region {
    int rx = 0, rz = 0;
    int x0 = 0, z0 = 0;                     // мировой угол региона
    std::vector<uint64_t> net;              // воздух сети
    std::vector<uint64_t> water;            // вода сети (воздух, касающийся воды)
    std::vector<uint64_t> sealed;           // закрытая пещера
    std::vector<uint64_t> flood;            // закрытая пещера, затопленная
};

namespace {

// Строитель региона. Все координаты, которые видит червь, мировые; в регион пишутся только
// вокселы внутри его границ. Высоты кэшируются по области региона с запасом.
class Builder {
public:
    Builder(const Cave_Network::Height_Fn& height, uint32_t seed,
            OpenSimplexEnv* env, OpenSimplexGradients* grad,
            Cave_Network::Region& region, int sea_level)
        : m_height(height), m_seed(seed), m_env(env), m_grad(grad),
          m_region(region), m_sea(sea_level)
    {
        m_ax = region.x0 - kMargin;
        m_az = region.z0 - kMargin;
        m_area = kRegion + 2 * kMargin;
        m_hval.assign(static_cast<size_t>(m_area) * m_area, 0);
        m_hset.assign(static_cast<size_t>(m_area) * m_area, 0);
    }

    void build() {
        const size_t net_bits = static_cast<size_t>(kRegion) * kRegion * kNetNY;
        const size_t sealed_bits = static_cast<size_t>(kRegion) * kRegion * kSealedNY;
        m_region.net.assign((net_bits + 63) / 64, 0);
        m_region.water.assign((net_bits + 63) / 64, 0);
        m_region.sealed.assign((sealed_bits + 63) / 64, 0);
        m_region.flood.assign((sealed_bits + 63) / 64, 0);

        std::vector<Worm> queue;
        spawn_worms(queue);
        for (size_t i = 0; i < queue.size(); ++i) {
            Worm w = queue[i];
            run_worm(w, queue);
        }
        spawn_sealed();
        mark_water();
    }

private:
    struct Worm {
        double x = 0, y = 0, z = 0;
        double yaw = 0, pitch = 0;
        double r = 2.0;
        int life = 100;
        int budget = 3;
        bool shaft = false;
        uint64_t seed = 1;
    };

    double n3(double x, double y, double z) const {
        return noise3_XZBeforeY(m_env, m_grad, x, y, z);
    }

    // Высота с кэшем по области региона. Вне области (редкий путь) — прямой вызов.
    int height(int wx, int wz) {
        const int lx = wx - m_ax, lz = wz - m_az;
        if (lx < 0 || lz < 0 || lx >= m_area || lz >= m_area) return m_height(wx, wz);
        const size_t i = static_cast<size_t>(lz) * m_area + lx;
        if (!m_hset[i]) {
            m_hval[i] = m_height(wx, wz);
            m_hset[i] = 1;
        }
        return m_hval[i];
    }

    bool in_region(int wx, int wz) const {
        return wx >= m_region.x0 && wx < m_region.x0 + kRegion &&
               wz >= m_region.z0 && wz < m_region.z0 + kRegion;
    }

    // Эллипсоид сети. Крыша: обычный ход держит kRoof блоков, шахта может срезать верхний блок.
    void carve_net(double cx, double cy, double cz, double rxz, double ry, bool shaft) {
        const int xa = std::max(m_region.x0, static_cast<int>(std::floor(cx - rxz)));
        const int xb = std::min(m_region.x0 + kRegion - 1, static_cast<int>(std::ceil(cx + rxz)));
        const int za = std::max(m_region.z0, static_cast<int>(std::floor(cz - rxz)));
        const int zb = std::min(m_region.z0 + kRegion - 1, static_cast<int>(std::ceil(cz + rxz)));
        const int ya = std::max(Cave_Network::kNetworkMinY, static_cast<int>(std::floor(cy - ry)));
        const int yb = std::min(Cave_Network::kNetworkMaxY, static_cast<int>(std::ceil(cy + ry)));
        for (int wz = za; wz <= zb; ++wz) {
            for (int wx = xa; wx <= xb; ++wx) {
                const int h = height(wx, wz);
                const int limit = shaft ? h : h - kRoof;
                for (int y = ya; y <= yb && y <= limit; ++y) {
                    const double dx = (wx + 0.5 - cx) / rxz;
                    const double dy = (y + 0.5 - cy) / ry;
                    const double dz = (wz + 0.5 - cz) / rxz;
                    if (dx * dx + dy * dy + dz * dz > 1.0) continue;
                    set_bit(m_region.net, net_index(wx - m_region.x0, y, wz - m_region.z0));
                }
            }
        }
    }

    void run_worm(Worm w, std::vector<Worm>& queue) {
        Rng rng(w.seed);
        int age = 0;
        while (w.life > 0) {
            const bool shaft = w.shaft && age < kShaftLen;

            const double nr = n3(w.x * 0.03, w.y * 0.03, w.z * 0.03 + 400.0);
            const double rr = w.r * (0.85 + 0.15 * nr);
            carve_net(w.x, w.y, w.z, rr, rr * 0.7, shaft);

            if (age > 20 && !shaft && rng.uni() < kChamberChance) {
                const double R = rng.range(4.5, 7.5);
                carve_net(w.x, w.y, w.z, R, R * 0.6, false);
            }

            w.yaw += 0.10 * n3(w.x * 0.02, w.y * 0.02 + 50.0, w.z * 0.02 + 900.0);
            if (shaft) {
                w.pitch += 0.25 * (-1.25 - w.pitch);
            } else {
                w.pitch += 0.05 * n3(w.x * 0.02 + 300.0, w.y * 0.02, w.z * 0.02 + 20.0);
                w.pitch = std::clamp(w.pitch, -0.4, 0.4);
            }
            const int h = height(static_cast<int>(std::floor(w.x)), static_cast<int>(std::floor(w.z)));
            if (w.y < Cave_Network::kNetworkMinY + 10) w.pitch = std::max(w.pitch, 0.25);
            if (!shaft && w.y > h - 14) w.pitch = std::min(w.pitch, -0.25);

            const double cp = std::cos(w.pitch);
            w.x += cp * std::cos(w.yaw);
            w.y += std::sin(w.pitch);
            w.z += cp * std::sin(w.yaw);
            --w.life;
            ++age;

            if (age > 10 && w.budget > 0 && rng.uni() < kBranchChance) {
                Worm c = w;
                c.yaw += rng.range(-1.6, 1.6);
                c.pitch = rng.range(-0.4, 0.4);
                c.r = w.r * 0.7;
                c.life = std::max(10, w.life / 2);
                c.budget = w.budget - 1;
                c.shaft = false;
                c.seed = rng.next();
                queue.push_back(c);
                w.budget -= 1;
            }
        }
    }

    // Черви спавнятся в мировых ячейках, которые покрывают область региона с запасом.
    // Состав ячейки зависит только от её мировых координат и seed.
    void spawn_worms(std::vector<Worm>& out) {
        const int cz0 = floordiv(m_az, kCell), cz1 = floordiv(m_az + m_area - 1, kCell);
        const int cx0 = floordiv(m_ax, kCell), cx1 = floordiv(m_ax + m_area - 1, kCell);
        for (int cz = cz0; cz <= cz1; ++cz) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                const uint64_t key = static_cast<uint64_t>(static_cast<int64_t>(cx)) * 73856093ULL ^
                                     static_cast<uint64_t>(static_cast<int64_t>(cz)) * 19349663ULL;
                Rng r(splitmix(static_cast<uint64_t>(m_seed) ^ key));
                const double expected = kWormsBase + kWormsExtra;
                int count = static_cast<int>(expected);
                if (r.uni() < expected - count) ++count;
                for (int k = 0; k < count; ++k) {
                    Worm w;
                    w.x = cx * kCell + r.range(0.0, kCell);
                    w.z = cz * kCell + r.range(0.0, kCell);
                    const int h = height(static_cast<int>(std::floor(w.x)), static_cast<int>(std::floor(w.z)));
                    if (r.uni() < kEntranceChance) {
                        w.y = h;
                        w.shaft = true;
                        w.pitch = -1.25;
                    } else {
                        w.y = r.range(Cave_Network::kNetworkMinY + 8, std::max(Cave_Network::kNetworkMinY + 9.0, h - 25.0));
                        w.shaft = false;
                        w.pitch = r.range(-0.3, 0.3);
                    }
                    w.yaw = r.range(0.0, 6.2831853);
                    w.r = r.range(1.6, 2.8);
                    w.life = static_cast<int>(r.range(90.0, 240.0));
                    w.budget = 3;
                    w.seed = r.next();
                    out.push_back(w);
                }
            }
        }
    }

    // Закрытые пещеры: одна камера на ячейку с вероятностью 1/2. Внутри своей ячейки и
    // запаса kSealedMargin; никогда не выше kSealedMaxY и не ниже kSealedMinY.
    void spawn_sealed() {
        const int cz0 = floordiv(m_region.z0 - kSealedMargin, kSealedCell);
        const int cz1 = floordiv(m_region.z0 + kRegion + kSealedMargin, kSealedCell);
        const int cx0 = floordiv(m_region.x0 - kSealedMargin, kSealedCell);
        const int cx1 = floordiv(m_region.x0 + kRegion + kSealedMargin, kSealedCell);
        for (int cz = cz0; cz <= cz1; ++cz) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                const uint64_t key = static_cast<uint64_t>(static_cast<int64_t>(cx)) * 83492791ULL ^
                                     static_cast<uint64_t>(static_cast<int64_t>(cz)) * 2971215073ULL;
                Rng r(splitmix(static_cast<uint64_t>(m_seed) ^ 0x5EA1EDULL ^ key));
                if (r.uni() > 0.5) continue;
                const double cxw = cx * kSealedCell + r.range(0.0, kSealedCell);
                const double czw = cz * kSealedCell + r.range(0.0, kSealedCell);
                const double rxz = r.range(10.0, 20.0);
                const double ry = r.range(7.0, 11.0);
                const double cy = r.range(Cave_Network::kSealedMinY + ry + 2,
                                          Cave_Network::kSealedMaxY - ry - 1);
                const bool flood = (cy - ry >= Cave_Network::kFloodFloorY) && r.uni() < 0.5;
                carve_sealed(cxw, cy, czw, rxz, ry, flood);
            }
        }
    }

    void carve_sealed(double cx, double cy, double cz, double rxz, double ry, bool flood) {
        const int xa = std::max(m_region.x0, static_cast<int>(std::floor(cx - rxz)));
        const int xb = std::min(m_region.x0 + kRegion - 1, static_cast<int>(std::ceil(cx + rxz)));
        const int za = std::max(m_region.z0, static_cast<int>(std::floor(cz - rxz)));
        const int zb = std::min(m_region.z0 + kRegion - 1, static_cast<int>(std::ceil(cz + rxz)));
        const int ya = std::max(Cave_Network::kSealedMinY, static_cast<int>(std::floor(cy - ry)));
        const int yb = std::min(Cave_Network::kSealedMaxY, static_cast<int>(std::ceil(cy + ry)));
        for (int wz = za; wz <= zb; ++wz) {
            for (int wx = xa; wx <= xb; ++wx) {
                for (int y = ya; y <= yb; ++y) {
                    const double dx = (wx + 0.5 - cx) / rxz;
                    const double dy = (y + 0.5 - cy) / ry;
                    const double dz = (wz + 0.5 - cz) / rxz;
                    if (dx * dx + dy * dy + dz * dz > 1.0) continue;
                    const size_t i = sealed_index(wx - m_region.x0, y, wz - m_region.z0);
                    set_bit(m_region.sealed, i);
                    if (flood) set_bit(m_region.flood, i);
                }
            }
        }
    }

    // Вода сети: воздух сети ниже уровня моря, который соприкасается с водяной колонкой.
    // Вода распространяется только по прямому контакту — это локальное, детерминированное
    // правило, не требующее обхода связности через границы регионов.
    void mark_water() {
        for (int lz = 0; lz < kRegion; ++lz) {
            for (int lx = 0; lx < kRegion; ++lx) {
                const int wx = m_region.x0 + lx, wz = m_region.z0 + lz;
                const int ymax = std::min(Cave_Network::kNetworkMaxY, m_sea);
                for (int y = Cave_Network::kNetworkMinY; y <= ymax; ++y) {
                    const size_t i = net_index(lx, y, lz);
                    if (!get_bit(m_region.net, i)) continue;
                    const int nx[6] = {wx + 1, wx - 1, wx, wx, wx, wx};
                    const int nz[6] = {wz, wz, wz, wz, wz + 1, wz - 1};
                    const int ny[6] = {y, y, y + 1, y - 1, y, y};
                    for (int k = 0; k < 6; ++k) {
                        if (ny[k] <= m_sea && ny[k] > height(nx[k], nz[k])) {
                            set_bit(m_region.water, i);
                            break;
                        }
                    }
                }
            }
        }
    }

    const Cave_Network::Height_Fn& m_height;
    uint32_t m_seed;
    OpenSimplexEnv* m_env;
    OpenSimplexGradients* m_grad;
    Cave_Network::Region& m_region;
    int m_sea;
    int m_ax = 0, m_az = 0, m_area = 0;
    std::vector<int> m_hval;
    std::vector<uint8_t> m_hset;
};

}  // namespace

// ---------------------------------------------------------------------------
//  Cave_Network
// ---------------------------------------------------------------------------
Cave_Network::Cave_Network(uint32_t seed, Height_Fn height, int sea_level)
    : m_id(g_next_id.fetch_add(1, std::memory_order_relaxed))
    , m_seed(seed)
    , m_sea_level(sea_level)
    , m_height(std::move(height))
{
    m_env = initOpenSimplex();
    m_grad = newOpenSimplexGradients(static_cast<OpenSimplexEnv*>(m_env), static_cast<long>(seed));
}

Cave_Network::~Cave_Network() = default;

void Cave_Network::clear_cache() const {
    m_regions.clear();
}

std::shared_ptr<const Cave_Network::Region> Cave_Network::fetch_region(int rx, int rz) const {
    const int64_t key = (static_cast<int64_t>(rx) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(rz));

    // Построение идёт без общего замка: см. Region_Cache.
    return m_regions.get(key, static_cast<size_t>(kCacheRegions), [&]() {
        auto region = std::make_shared<Region>();
        region->rx = rx;
        region->rz = rz;
        region->x0 = rx * kRegion;
        region->z0 = rz * kRegion;
        Builder builder(m_height, m_seed, static_cast<OpenSimplexEnv*>(m_env),
                        static_cast<OpenSimplexGradients*>(m_grad), *region, m_sea_level);
        builder.build();
        return region;
    });
}

Cave_Kind Cave_Network::kind_at(int world_x, int world_y, int world_z) const {
    const bool in_network = world_y >= kNetworkMinY && world_y <= kNetworkMaxY;
    const bool in_sealed = world_y >= kSealedMinY && world_y <= kSealedMaxY;
    if (!in_network && !in_sealed) return Cave_Kind::None;

    const int rx = floordiv(world_x, kRegion);
    const int rz = floordiv(world_z, kRegion);

    // Последний регион каждого потока: большинство вызовов идёт внутри одного региона.
    struct Last_Region {
        uint64_t owner = 0;
        int rx = 0, rz = 0;
        std::shared_ptr<const Region> region;
    };
    thread_local Last_Region last;
    if (!(last.region && last.owner == m_id && last.rx == rx && last.rz == rz)) {
        last.region = fetch_region(rx, rz);
        last.owner = m_id;
        last.rx = rx;
        last.rz = rz;
    }
    const Region& r = *last.region;
    const int lx = world_x - r.x0;
    const int lz = world_z - r.z0;

    if (in_network) {
        const size_t i = net_index(lx, world_y, lz);
        if (get_bit(r.net, i)) return get_bit(r.water, i) ? Cave_Kind::Water : Cave_Kind::Air;
    }
    if (in_sealed) {
        const size_t i = sealed_index(lx, world_y, lz);
        if (get_bit(r.sealed, i)) return get_bit(r.flood, i) ? Cave_Kind::Sealed_Flooded : Cave_Kind::Sealed_Dry;
    }
    return Cave_Kind::None;
}
