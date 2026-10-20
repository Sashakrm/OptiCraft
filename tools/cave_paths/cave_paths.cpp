// Прототип пещерной сети на путях ("червях"). Отдельная утилита, не тянет Chunk/Renderer/OpenGL.
// Использует ТОТ ЖЕ OpenSimplex2S, что и движок. Рельеф — заглушка (HW).
//
// Детерминированность: черви спавнятся в мировых ячейках CELL, каждый червь имеет собственный
// seed (от ячейки), вся симуляция идёт в мировых координатах. Окно (g_ox,g_oz,W,D) только
// ограничивает вывод. Два окна с разным сдвигом должны давать одинаковые вокселы на перекрытии:
// проверяется через --dump (см. README в конце файла / build.sh).
//
// Правила (согласованы с автором):
//   * сеть пещер живёт между Y_MIN = -60 и поверхностью; ниже -60 червь ничего не вырезает;
//   * входы на поверхность и выходы в воду — редкие, но есть;
//   * протяжённые ходы с ответвлениями и редкими залами, без порога шума.
//
// Выход: статистика в stdout, PNG-срезы и проекция в out/, опционально дамп прямоугольника.

#include "OpenSimplex2S.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---- Окно вывода и правила ----------------------------------------------------------------
constexpr int W = 384;          // ширина окна по X
constexpr int D = 384;          // ширина окна по Z
constexpr int Y_MIN = -60;      // сеть не опускается ниже
constexpr int Y_MAX = 130;      // потолок расчётного объёма
constexpr int NY = Y_MAX - Y_MIN + 1;
constexpr int SEA = 63;         // уровень моря
constexpr int ROOF = 5;         // крыша под поверхностью (кроме шахт)
constexpr int SHAFT_LEN = 14;   // сколько первых шагов шахта может резать у поверхности
constexpr int CELL = 64;        // шаг мировых ячеек для спавна червей
// Запас спавна за окном: червь + ответвления не могут уйти дальше, иначе окно не воспроизводимо.
constexpr int MARGIN = 400;

// Настраиваемые числа.
double g_entrance_chance = 0.9;   // доля червей, начинающих шахтой у поверхности
double g_worms_base = 1.0;        // минимум червей на ячейку
double g_worms_extra = 1.0;       // ожидаемое число дополнительных червей на ячейку
double g_chamber_chance = 0.012;  // шанс зала на шаг (после 20 шагов)
double g_branch_chance = 0.008;   // шанс ответвления на шаг
int    g_seed = 1327;

// Мировые координаты левого-нижнего угла окна и параметры дампа.
int g_ox = 0, g_oz = 0;
std::string g_dump_path;
int g_dump_x0 = 0, g_dump_z0 = 0, g_dump_x1 = 0, g_dump_z1 = 0;

OpenSimplexEnv* g_env = nullptr;
OpenSimplexGradients* g_grad = nullptr;

inline size_t vidx(int x, int y, int z) {   // локальные координаты окна
    return (static_cast<size_t>(y - Y_MIN) * D + z) * W + x;
}
inline bool in_xz(int x, int z) { return x >= 0 && x < W && z >= 0 && z < D; }

// ---- Детерминированный RNG (splitmix) -----------------------------------------------------
uint64_t splitmix(uint64_t v) {
    v += 0x9E3779B97F4A7C15ULL;
    v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ULL;
    v = (v ^ (v >> 27)) * 0x94D049BB133111EBULL;
    return v ^ (v >> 31);
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() { return s = splitmix(s); }
    double uni() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double range(double a, double b) { return a + (b - a) * uni(); }
    int irange(int a, int b) { return a + static_cast<int>(next() % static_cast<uint64_t>(b - a + 1)); }
};

// ---- Шум ----------------------------------------------------------------------------------
double n3(double x, double y, double z) { return noise3_XZBeforeY(g_env, g_grad, x, y, z); }
double n2(double x, double z) { return noise2(g_env, g_grad, x, z); }

// ---- Рельеф-заглушка (мировые координаты) -------------------------------------------------
int HW(int wx, int wz) {
    const double e = 0.7 * n2(wx / 140.0, wz / 140.0) + 0.3 * n2(wx / 45.0 + 97.0, wz / 45.0 + 31.0);
    return 74 + static_cast<int>(std::lround(e * 22.0));
}

// Кэш для окна: H(x,z) по локальным координатам.
std::vector<int> g_height;
inline int H(int x, int z) { return g_height[static_cast<size_t>(z) * W + x]; }

void build_height() {
    g_height.assign(static_cast<size_t>(W) * D, 0);
    for (int z = 0; z < D; ++z)
        for (int x = 0; x < W; ++x)
            g_height[static_cast<size_t>(z) * W + x] = HW(g_ox + x, g_oz + z);
}

bool is_water(int x, int y, int z) { return in_xz(x, z) && y <= SEA && y > H(x, z); }
bool is_open_air(int x, int y, int z) { return in_xz(x, z) && y > H(x, z) && y > SEA; }

// ---- Вырезание ----------------------------------------------------------------------------
std::vector<uint8_t> g_carved;

// Эллипсоид в мировых координатах. Правила: ниже Y_MIN не режем; над поверхностью не режем;
// крыша ROOF блоков (шахта может срезать верхний блок H — это и есть вход).
void carve_ellipsoid(double wcx, double cy, double wcz, double rxz, double ry, bool shaft) {
    const double cx = wcx - g_ox, cz = wcz - g_oz;   // локальный центр
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - rxz)));
    const int x1 = std::min(W - 1, static_cast<int>(std::ceil(cx + rxz)));
    const int z0 = std::max(0, static_cast<int>(std::floor(cz - rxz)));
    const int z1 = std::min(D - 1, static_cast<int>(std::ceil(cz + rxz)));
    const int y0 = std::max(Y_MIN, static_cast<int>(std::floor(cy - ry)));
    const int y1 = std::min(Y_MAX, static_cast<int>(std::ceil(cy + ry)));
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const int h = H(x, z);
            const int limit = shaft ? h : h - ROOF;
            for (int y = y0; y <= y1; ++y) {
                if (y > limit) break;
                const double dx = (x + 0.5 - cx) / rxz;
                const double dy = (y + 0.5 - cy) / ry;
                const double dz = (z + 0.5 - cz) / rxz;
                if (dx * dx + dy * dy + dz * dz > 1.0) continue;
                g_carved[vidx(x, y, z)] = 1;
            }
        }
    }
}

// ---- Черви (мировые координаты, собственный RNG) ------------------------------------------
struct Worm {
    double x = 0, y = 0, z = 0;
    double yaw = 0, pitch = 0;
    double r = 2.0;
    int life = 100;
    int budget = 3;       // сколько ещё ответвлений может породить
    bool shaft = false;
    uint64_t seed = 1;    // собственный поток случайности
};

void run_worm(Worm w, std::vector<Worm>& queue) {
    Rng rng(w.seed);
    int age = 0;
    while (w.life > 0) {
        const bool shaft = w.shaft && age < SHAFT_LEN;

        const double nr = n3(w.x * 0.03, w.y * 0.03, w.z * 0.03 + 400.0);
        const double rr = w.r * (0.85 + 0.15 * nr);
        carve_ellipsoid(w.x, w.y, w.z, rr, rr * 0.7, shaft);

        if (age > 20 && !shaft && rng.uni() < g_chamber_chance) {
            const double R = rng.range(4.5, 7.5);
            carve_ellipsoid(w.x, w.y, w.z, R, R * 0.6, false);
        }

        // Рулевое. Высоту берём из мирового шума, чтобы не зависеть от окна.
        w.yaw += 0.10 * n3(w.x * 0.02, w.y * 0.02 + 50.0, w.z * 0.02 + 900.0);
        if (shaft) {
            w.pitch += 0.25 * (-1.25 - w.pitch);
        } else {
            w.pitch += 0.05 * n3(w.x * 0.02 + 300.0, w.y * 0.02, w.z * 0.02 + 20.0);
            w.pitch = std::clamp(w.pitch, -0.4, 0.4);
        }
        const int h = HW(static_cast<int>(std::floor(w.x)), static_cast<int>(std::floor(w.z)));
        if (w.y < Y_MIN + 10) w.pitch = std::max(w.pitch, 0.25);
        if (!shaft && w.y > h - 14) w.pitch = std::min(w.pitch, -0.25);

        const double cp = std::cos(w.pitch);
        w.x += cp * std::cos(w.yaw);
        w.y += std::sin(w.pitch);
        w.z += cp * std::sin(w.yaw);
        --w.life;
        ++age;

        // Ответвление: новый червь из текущей точки, другой курс, меньший радиус.
        if (age > 10 && w.budget > 0 && rng.uni() < g_branch_chance) {
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

inline int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Черви спавнятся по всем мировым ячейкам, которые могут достать до окна (с запасом MARGIN).
// Состав червей в ячейке зависит только от мировых координат ячейки и seed.
std::vector<Worm> spawn_worms() {
    std::vector<Worm> out;
    const int cz0 = floordiv(g_oz - MARGIN, CELL), cz1 = floordiv(g_oz + D + MARGIN, CELL);
    const int cx0 = floordiv(g_ox - MARGIN, CELL), cx1 = floordiv(g_ox + W + MARGIN, CELL);
    for (int cz = cz0; cz <= cz1; ++cz) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            const uint64_t key = static_cast<uint64_t>(static_cast<int64_t>(cx)) * 73856093ULL ^
                                 static_cast<uint64_t>(static_cast<int64_t>(cz)) * 19349663ULL;
            Rng r(splitmix(static_cast<uint64_t>(g_seed) ^ key));
            const double expected = g_worms_base + g_worms_extra;
            int count = static_cast<int>(expected);
            if (r.uni() < expected - count) ++count;
            for (int k = 0; k < count; ++k) {
                Worm w;
                w.x = cx * CELL + r.range(0.0, CELL);
                w.z = cz * CELL + r.range(0.0, CELL);
                const int h = HW(static_cast<int>(std::floor(w.x)), static_cast<int>(std::floor(w.z)));
                if (r.uni() < g_entrance_chance) {
                    w.y = h;
                    w.shaft = true;
                    w.pitch = -1.25;
                } else {
                    w.y = r.range(Y_MIN + 8, std::max(Y_MIN + 9.0, h - 25.0));
                    w.shaft = false;
                    w.pitch = r.range(-0.3, 0.3);
                }
                w.yaw = r.range(0.0, 6.2831853);
                w.r = r.range(1.6, 2.8);
                w.life = r.irange(90, 240);
                w.budget = 3;
                w.seed = r.next();
                out.push_back(w);
            }
        }
    }
    return out;
}

// ---- Анализ связности (только внутри окна) -----------------------------------------------
struct Comp {
    long voxels = 0;
    bool to_air = false;
    bool to_water = false;
};

struct Stats {
    long carved = 0;
    int comps_total = 0;
    int comps_big = 0;
    long largest = 0;
    long surface_volume = 0;
    int air_entrances = 0;
    int water_entrances = 0;
    int min_y = 0, max_y = 0;
    long openings = 0;
};

void check_neighbor(int x, int y, int z, Comp& c) {
    const int nx[6] = {x + 1, x - 1, x, x, x, x};
    const int ny[6] = {y, y, y + 1, y - 1, y, y};
    const int nz[6] = {z, z, z, z, z + 1, z - 1};
    for (int i = 0; i < 6; ++i) {
        if (is_water(nx[i], ny[i], nz[i])) c.to_water = true;
        if (is_open_air(nx[i], ny[i], nz[i])) c.to_air = true;
    }
}

Stats analyze(std::vector<int>& comp_id, std::vector<Comp>& comps) {
    Stats st;
    comp_id.assign(g_carved.size(), -1);
    std::vector<size_t> stack;
    st.min_y = Y_MAX;
    st.max_y = Y_MIN;
    for (size_t i = 0; i < g_carved.size(); ++i) {
        if (!g_carved[i]) continue;
        ++st.carved;
        if (comp_id[i] >= 0) continue;
        const int id = static_cast<int>(comps.size());
        comps.push_back(Comp{});
        comp_id[i] = id;
        stack.clear();
        stack.push_back(i);
        while (!stack.empty()) {
            const size_t c = stack.back();
            stack.pop_back();
            const int x = static_cast<int>(c % W);
            const int z = static_cast<int>((c / W) % D);
            const int y = static_cast<int>(c / (static_cast<size_t>(W) * D)) + Y_MIN;
            Comp& comp = comps[id];
            ++comp.voxels;
            st.min_y = std::min(st.min_y, y);
            st.max_y = std::max(st.max_y, y);
            check_neighbor(x, y, z, comp);

            const int nx[6] = {x + 1, x - 1, x, x, x, x};
            const int ny[6] = {y, y, y + 1, y - 1, y, y};
            const int nz[6] = {z, z, z, z, z + 1, z - 1};
            for (int k = 0; k < 6; ++k) {
                if (!in_xz(nx[k], nz[k])) continue;
                if (ny[k] < Y_MIN || ny[k] > Y_MAX) continue;
                const size_t n = vidx(nx[k], ny[k], nz[k]);
                if (!g_carved[n] || comp_id[n] >= 0) continue;
                comp_id[n] = id;
                stack.push_back(n);
            }
        }
    }

    for (const Comp& c : comps) {
        if (c.voxels >= 8) ++st.comps_big;
        if (c.voxels > st.largest) st.largest = c.voxels;
        if (c.to_air) ++st.air_entrances;
        if (c.to_water) ++st.water_entrances;
        if (c.to_air || c.to_water) st.surface_volume += c.voxels;
    }
    st.comps_total = static_cast<int>(comps.size());
    for (int z = 0; z < D; ++z)
        for (int x = 0; x < W; ++x) {
            const int h = H(x, z);
            if (h < Y_MIN || h > Y_MAX) continue;
            if (g_carved[vidx(x, h, z)]) ++st.openings;
        }
    return st;
}

// ---- Закрытые большие пещеры ниже -60 (пока окно-локальные, см. TODO в README) -----------
constexpr int SY_MIN = -150;
constexpr int SY_MAX = -63;
constexpr int SNY = SY_MAX - SY_MIN + 1;
constexpr int FLOOD_FLOOR = -120;
constexpr int SEALED_CELL = 96;
std::vector<uint8_t> g_sealed;  // 0 — камень, 1 — сухая закрытая пещера, 2 — затопленная

inline size_t sidx(int x, int y, int z) {
    return (static_cast<size_t>(y - SY_MIN) * D + z) * W + x;
}

struct SealedStats {
    int chambers = 0, dry = 0, flooded = 0;
    long dry_vox = 0, flooded_vox = 0;
    long flooded_below_floor = 0;
    long touching_network = 0;
};

void build_sealed(SealedStats& st) {
    g_sealed.assign(static_cast<size_t>(W) * D * SNY, 0);
    for (int cz = 0; cz * SEALED_CELL < D; ++cz) {
        for (int cx = 0; cx * SEALED_CELL < W; ++cx) {
            Rng r(splitmix(static_cast<uint64_t>(g_seed) ^ 0x5EA1ED ^ (static_cast<uint64_t>(cx) << 24) ^
                           (static_cast<uint64_t>(cz) << 44)));
            if (r.uni() > 0.5) continue;
            const double cxw = cx * SEALED_CELL + r.range(0, SEALED_CELL);
            const double czw = cz * SEALED_CELL + r.range(0, SEALED_CELL);
            const double rxz = r.range(10.0, 20.0);
            const double ry = r.range(7.0, 11.0);
            const double cy = r.range(SY_MIN + ry + 2, SY_MAX - ry - 1);
            const bool flood = (cy - ry >= FLOOD_FLOOR) && r.uni() < 0.5;
            ++st.chambers;
            (flood ? st.flooded : st.dry) += 1;
            const uint8_t kind = flood ? 2 : 1;
            for (int y = std::max(SY_MIN, static_cast<int>(std::floor(cy - ry)));
                 y <= std::min(SY_MAX, static_cast<int>(std::ceil(cy + ry))); ++y) {
                for (int z = std::max(0, static_cast<int>(std::floor(czw - rxz)));
                     z <= std::min(D - 1, static_cast<int>(std::ceil(czw + rxz))); ++z) {
                    for (int x = std::max(0, static_cast<int>(std::floor(cxw - rxz)));
                         x <= std::min(W - 1, static_cast<int>(std::ceil(cxw + rxz))); ++x) {
                        const double dx = (x + 0.5 - cxw) / rxz, dy = (y + 0.5 - cy) / ry, dz = (z + 0.5 - czw) / rxz;
                        if (dx * dx + dy * dy + dz * dz > 1.0) continue;
                        g_sealed[sidx(x, y, z)] = kind;
                        if (kind == 2) ++st.flooded_vox; else ++st.dry_vox;
                        if (kind == 2 && y < FLOOD_FLOOR) ++st.flooded_below_floor;
                    }
                }
            }
        }
    }
    for (int y = SY_MIN; y <= SY_MAX; ++y)
        for (int z = 0; z < D; ++z)
            for (int x = 0; x < W; ++x) {
                if (!g_sealed[sidx(x, y, z)]) continue;
                const int nx[6] = {x + 1, x - 1, x, x, x, x}, nz[6] = {z, z, z, z, z + 1, z - 1};
                const int ny[6] = {y, y, y + 1, y - 1, y, y};
                for (int k = 0; k < 6; ++k) {
                    if (!in_xz(nx[k], nz[k]) || ny[k] < Y_MIN || ny[k] > Y_MAX) continue;
                    if (g_carved[vidx(nx[k], ny[k], nz[k])]) ++st.touching_network;
                }
            }
}

// ---- Картинки -----------------------------------------------------------------------------
void rgb_for_comp(int id, uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint64_t h = splitmix(static_cast<uint64_t>(id) * 2654435761ULL + 17);
    r = static_cast<uint8_t>(90 + (h & 0x7F));
    g = static_cast<uint8_t>(90 + ((h >> 8) & 0x7F));
    b = static_cast<uint8_t>(90 + ((h >> 16) & 0x7F));
}

void write_xz_slice(const std::string& path, int y, const std::vector<int>& comp_id) {
    std::vector<uint8_t> img(static_cast<size_t>(W) * D * 3);
    for (int z = 0; z < D; ++z)
        for (int x = 0; x < W; ++x) {
            uint8_t r = 40, g = 40, b = 40;
            if (is_water(x, y, z)) { r = 40; g = 90; b = 180; }
            if (is_open_air(x, y, z)) { r = 200; g = 220; b = 240; }
            if (y >= Y_MIN && y <= Y_MAX && g_carved[vidx(x, y, z)]) rgb_for_comp(comp_id[vidx(x, y, z)], r, g, b);
            const size_t p = (static_cast<size_t>(z) * W + x) * 3;
            img[p] = r; img[p + 1] = g; img[p + 2] = b;
        }
    stbi_write_png(path.c_str(), W, D, 3, img.data(), W * 3);
}

void write_xy_slice(const std::string& path, int z, const std::vector<int>& comp_id) {
    std::vector<uint8_t> img(static_cast<size_t>(W) * NY * 3);
    for (int row = 0; row < NY; ++row) {
        const int y = Y_MAX - row;
        for (int x = 0; x < W; ++x) {
            uint8_t r = 40, g = 40, b = 40;
            if (is_water(x, y, z)) { r = 40; g = 90; b = 180; }
            if (is_open_air(x, y, z)) { r = 200; g = 220; b = 240; }
            if (y >= Y_MIN && y <= Y_MAX && g_carved[vidx(x, y, z)]) rgb_for_comp(comp_id[vidx(x, y, z)], r, g, b);
            const size_t p = (static_cast<size_t>(row) * W + x) * 3;
            img[p] = r; img[p + 1] = g; img[p + 2] = b;
        }
    }
    stbi_write_png(path.c_str(), W, NY, 3, img.data(), W * 3);
}

void write_projection(const std::string& path, const std::vector<int>& comp_id) {
    std::vector<uint8_t> img(static_cast<size_t>(W) * D * 3, 30);
    for (int z = 0; z < D; ++z)
        for (int x = 0; x < W; ++x) {
            uint8_t r = 45, g = 45, b = 45;
            const int h = H(x, z);
            for (int y = std::min(h, Y_MAX); y >= Y_MIN; --y) {
                if (!g_carved[vidx(x, y, z)]) continue;
                rgb_for_comp(comp_id[vidx(x, y, z)], r, g, b);
                if (y == h) { r = 255; g = 255; b = 255; }
                break;
            }
            const size_t p = (static_cast<size_t>(z) * W + x) * 3;
            img[p] = r; img[p + 1] = g; img[p + 2] = b;
        }
    stbi_write_png(path.c_str(), W, D, 3, img.data(), W * 3);
}

// Дамп прямоугольника в МИРОВЫХ координатах: для каждого (z,x,y) — байт 0/1. Два окна,
// перекрывающих прямоугольник, должны дать одинаковые файлы.
void dump_rect(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    for (int wz = g_dump_z0; wz < g_dump_z1; ++wz)
        for (int wx = g_dump_x0; wx < g_dump_x1; ++wx)
            for (int y = Y_MIN; y <= Y_MAX; ++y) {
                const int lx = wx - g_ox, lz = wz - g_oz;
                uint8_t v = 0;
                if (in_xz(lx, lz)) v = g_carved[vidx(lx, y, lz)];
                else v = 255;  // вне окна: в сравнении такие байты должны отсутствовать
                std::fwrite(&v, 1, 1, f);
            }
    std::fclose(f);
}

// ---- Аргументы ----------------------------------------------------------------------------
void parse_args(int argc, char** argv, std::string& out_dir) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--seed") g_seed = std::atoi(next());
        else if (a == "--entrance") g_entrance_chance = std::atof(next());
        else if (a == "--worms") g_worms_extra = std::atof(next());
        else if (a == "--chamber") g_chamber_chance = std::atof(next());
        else if (a == "--branch") g_branch_chance = std::atof(next());
        else if (a == "--origin") { g_ox = std::atoi(next()); g_oz = std::atoi(next()); }
        else if (a == "--dump") {
            g_dump_path = next();
            g_dump_x0 = std::atoi(next()); g_dump_z0 = std::atoi(next());
            g_dump_x1 = std::atoi(next()); g_dump_z1 = std::atoi(next());
        }
        else if (a == "--out") out_dir = next();
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir = ".";
    parse_args(argc, argv, out_dir);

    g_env = initOpenSimplex();
    g_grad = newOpenSimplexGradients(g_env, g_seed);
    build_height();
    g_carved.assign(static_cast<size_t>(W) * D * NY, 0);

    std::vector<Worm> queue = spawn_worms();
    for (size_t i = 0; i < queue.size(); ++i) {
        Worm w = queue[i];
        run_worm(w, queue);
    }

    if (!g_dump_path.empty()) {
        dump_rect(g_dump_path);
        std::printf("dumped world rect [%d..%d)x[%d..%d) to %s\n", g_dump_x0, g_dump_x1,
                    g_dump_z0, g_dump_z1, g_dump_path.c_str());
    }

    SealedStats sst;
    build_sealed(sst);

    std::vector<int> comp_id;
    std::vector<Comp> comps;
    const Stats st = analyze(comp_id, comps);

    const double columns16 = (W / 16.0) * (D / 16.0);
    std::printf("seed=%d window origin=(%d,%d) size=%dx%d y=[%d..%d]\n", g_seed, g_ox, g_oz, W, D, Y_MIN, Y_MAX);
    std::printf("worms=%zu chamber=%.4f branch=%.4f entrance=%.2f\n", queue.size(),
                g_chamber_chance, g_branch_chance, g_entrance_chance);
    std::printf("carved voxels: %ld (min y %d, max y %d)\n", st.carved, st.min_y, st.max_y);
    std::printf("networks: total %d, >=8 vox %d, largest %ld (%.1f%% of carved)\n",
                st.comps_total, st.comps_big, st.largest,
                st.carved ? 100.0 * st.largest / st.carved : 0.0);
    std::printf("in networks touching surface/water: %.1f%% of carved\n",
                st.carved ? 100.0 * st.surface_volume / st.carved : 0.0);
    std::printf("air entrances (networks): %d -> %.3f per 16x16 column\n",
                st.air_entrances, st.air_entrances / columns16);
    std::printf("water entrances (networks): %d\n", st.water_entrances);
    std::printf("openings (columns with cave at surface): %ld -> %.3f per 16x16 column\n",
                st.openings, st.openings / columns16);
    std::printf("constraint y >= %d: %s\n", Y_MIN, st.min_y >= Y_MIN ? "OK" : "VIOLATED");
    std::printf("sealed caves below %d: %d (dry %d, flooded %d), voxels dry %ld flooded %ld\n",
                Y_MIN, sst.chambers, sst.dry, sst.flooded, sst.dry_vox, sst.flooded_vox);
    std::printf("sealed checks: touching network %ld (must be 0), flooded below %d: %ld (must be 0)\n",
                sst.touching_network, FLOOD_FLOOR, sst.flooded_below_floor);
    std::printf("debug: queue (incl. branches) %zu\n", queue.size());

    write_xz_slice(out_dir + "/cave_xz_y-20.png", -20, comp_id);
    write_xy_slice(out_dir + "/cave_xy_z192.png", D / 2, comp_id);
    write_projection(out_dir + "/cave_projection.png", comp_id);
    std::printf("wrote slices and projection to %s\n", out_dir.c_str());
    return 0;
}
