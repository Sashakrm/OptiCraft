#include "world/generation/Maze.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stack>
#include <utility>

namespace {
    // Направления: 0 = N (-z), 1 = E (+x), 2 = S (+z), 3 = W (-x).
    constexpr int k_dx[4] = {0, 1, 0, -1};
    constexpr int k_dy[4] = {-1, 0, 1, 0};
    constexpr uint8_t k_opposite[4] = {0b0100, 0b1000, 0b0001, 0b0010};

    int rnd(Fast_Rng& rng, int lo, int hi) {
        std::uniform_int_distribution<int> d(lo, hi);
        return d(rng);
    }

    struct Dsu {
        std::vector<int> parent;
        std::vector<int> rank;

        explicit Dsu(int n) : parent(n), rank(n, 0) { std::iota(parent.begin(), parent.end(), 0); }

        int find(int v) { return parent[v] == v ? v : parent[v] = find(parent[v]); }

        void unite(int a, int b) {
            a = find(a);
            b = find(b);
            if (a == b) return;
            if (rank[a] < rank[b]) std::swap(a, b);
            parent[b] = a;
            if (rank[a] == rank[b]) ++rank[a];
        }
    };

    // Размеры этажа: случайная пара (W, H) из допустимых; fallback — почти квадрат.
    std::pair<int, int> pick_dimensions(const Maze_Config& c, Fast_Rng& rng) {
        std::vector<std::pair<int, int>> valid;
        for (int w = c.min_side; w <= c.max_side; ++w) {
            for (int h = c.min_side; h <= c.max_side; ++h) {
                const int area = w * h;
                if (area < c.min_area || area > c.max_area) continue;
                const int mn = std::min(w, h);
                const int mx = std::max(w, h);
                if (mx > c.max_aspect * mn) continue;
                valid.emplace_back(w, h);
            }
        }
        if (valid.empty()) {
            const int area = (c.min_area + c.max_area) / 2;
            const int w = std::max(c.min_side, static_cast<int>(std::round(std::sqrt(static_cast<double>(area)))));
            const int h = std::max(c.min_side, area / w);
            return {w, h};
        }
        return valid[rnd(rng, 0, static_cast<int>(valid.size()) - 1)];
    }

    // Recursive backtracker: идеальный лабиринт на сетке W x H.
    void carve_floor(Maze_Floor& f, Fast_Rng& rng) {
        std::vector<char> visited(f.W * f.H, 0);
        std::stack<std::pair<int, int>> st;

        const int sx = rnd(rng, 0, f.W - 1);
        const int sy = rnd(rng, 0, f.H - 1);
        visited[sy * f.W + sx] = 1;
        st.push({sx, sy});

        while (!st.empty()) {
            const auto [x, y] = st.top();
            std::vector<int> dirs;
            for (int d = 0; d < 4; ++d) {
                const int nx = x + k_dx[d];
                const int ny = y + k_dy[d];
                if (nx < 0 || ny < 0 || nx >= f.W || ny >= f.H) continue;
                if (!visited[ny * f.W + nx]) dirs.push_back(d);
            }
            if (dirs.empty()) {
                st.pop();
                continue;
            }

            const int d = dirs[rnd(rng, 0, static_cast<int>(dirs.size()) - 1)];
            const int nx = x + k_dx[d];
            const int ny = y + k_dy[d];

            f.at(x, y).walls &= static_cast<uint8_t>(~(1u << d));
            f.at(nx, ny).walls &= static_cast<uint8_t>(~k_opposite[d]);

            visited[ny * f.W + nx] = 1;
            st.push({nx, ny});
        }
    }

    // Минимум спусков с этажа f (f >= 1).
    int min_descents(int f) {
        if (f <= 0) return 0;
        int n = 1;
        if (f >= 5) n = 2;
        if (f >= 7) n = 3;
        return std::min(n, f);
    }

    // Разрешён ли прямой портал f -> t (t < f): на соседний этаж всегда, на f-2 — с 5-го этажа.
    bool portal_allowed(int f, int t) {
        if (t >= f) return false;
        if (t == f - 1) return true;
        return t == f - 2 && f >= 5;
    }

    // Кольцо спуска занимает 2x2 клетки (см. place): (x,y), (x+sx,y), (x+sx,y+sy), (x,y+sy).
    bool ring_clash(const Maze& m, int f, int t, int idx) {
        const int x = idx % m.W;
        const int y = idx / m.W;
        const int sx = (x + 1 < m.W) ? 1 : -1;
        const int sy = (y + 1 < m.H) ? 1 : -1;
        for (const Maze_Portal& p : m.portals) {
            // Диапазоны этажей пересекаются (делят хотя бы один этаж) — кольца стоят на одних уровнях.
            const bool levels_overlap = !(t > p.from_floor || f < p.to_floor);
            if (!levels_overlap) continue;
            const int px = p.x, py = p.y;
            const int psx = (px + 1 < m.W) ? 1 : -1;
            const int psy = (py + 1 < m.H) ? 1 : -1;
            for (int ax = 0; ax <= 1; ++ax)
                for (int ay = 0; ay <= 1; ++ay)
                    for (int bx = 0; bx <= 1; ++bx)
                        for (int by = 0; by <= 1; ++by) {
                            const int cx1 = x + (ax ? sx : 0), cy1 = y + (ay ? sy : 0);
                            const int cx2 = px + (bx ? psx : 0), cy2 = py + (by ? psy : 0);
                            if (cx1 == cx2 && cy1 == cy2) return true;
                        }
        }
        return false;
    }

    // Узел графа: (этаж, клетка) -> f * W*H + idx. Рёбра — проходы внутри этажей и порталы.
    void build_dsu(const Maze& m, Dsu& dsu) {
        const int W = m.W;
        const int H = m.H;
        const int per_floor = W * H;
        for (int f = 0; f < static_cast<int>(m.floors.size()); ++f) {
            const Maze_Floor& fl = m.floors[f];
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    const int id = f * per_floor + y * W + x;
                    if (x + 1 < W && fl.can_go(x, y, 1)) dsu.unite(id, f * per_floor + y * W + x + 1);
                    if (y + 1 < H && fl.can_go(x, y, 2)) dsu.unite(id, f * per_floor + (y + 1) * W + x);
                }
            }
        }
        for (const Maze_Portal& p : m.portals) {
            const int idx = p.y * W + p.x;
            dsu.unite(p.from_floor * per_floor + idx, p.to_floor * per_floor + idx);
        }
    }
} // namespace

Maze Maze_Generator::generate(const Maze_Config& config, Fast_Rng& rng) {
    const int F = rnd(rng, config.min_floors, config.max_floors);
    const auto [W, H] = pick_dimensions(config, rng);

    Maze m;
    m.W = W;
    m.H = H;
    m.floors.resize(F);
    for (Maze_Floor& f : m.floors) {
        f.W = W;
        f.H = H;
        f.cells.assign(W * H, Maze_Cell{});
        carve_floor(f, rng);
    }

    // Клетка может быть занята не более чем одним порталом на каждом этаже.
    std::vector<std::vector<char>> used(F, std::vector<char>(W * H, 0));

    auto try_add_portal = [&](int f, int t) -> bool {
        if (!portal_allowed(f, t)) return false;
        std::vector<int> idxs(W * H);
        std::iota(idxs.begin(), idxs.end(), 0);
        std::shuffle(idxs.begin(), idxs.end(), rng);
        for (int idx : idxs) {
            if (used[f][idx] || used[t][idx]) continue;
            if (ring_clash(m, f, t, idx)) continue;
            used[f][idx] = used[t][idx] = 1;
            m.portals.push_back({f, t, idx % W, idx / W});
            return true;
        }
        return false;
    };

    // 1) Базовые спуски по правилам.
    for (int f = 1; f < F; ++f) {
        const int need = min_descents(f);
        int have = 0;
        for (int t = f - 1; t >= 0 && have < need; --t) {
            if (try_add_portal(f, t)) ++have;
        }
    }

    // 2) Сшивка компонент связности легальными порталами (жадно: между двумя крупнейшими).
    const int per_floor = W * H;
    const int total = F * per_floor;
    for (int guard = 0; guard < 100000; ++guard) {
        Dsu dsu(total);
        build_dsu(m, dsu);

        std::vector<int> comp_size(total, 0);
        int comp_count = 0;
        for (int i = 0; i < total; ++i) {
            const int r = dsu.find(i);
            if (comp_size[r]++ == 0) ++comp_count;
        }
        if (comp_count <= 1) break;

        struct Candidate {
            int f, t, idx, score;
        };
        bool have_best = false;
        Candidate best{};
        for (int f = 1; f < F; ++f) {
            for (int t = f - 1; t >= 0; --t) {
                if (!portal_allowed(f, t)) continue;
                for (int idx = 0; idx < per_floor; ++idx) {
                    if (used[f][idx] || used[t][idx]) continue;
                    if (ring_clash(m, f, t, idx)) continue;
                    const int ra = dsu.find(f * per_floor + idx);
                    const int rb = dsu.find(t * per_floor + idx);
                    if (ra == rb) continue;
                    const int score = comp_size[ra] + comp_size[rb];
                    if (!have_best || score > best.score) {
                        best = {f, t, idx, score};
                        have_best = true;
                    }
                }
            }
        }
        if (!have_best) break;   // легальных соединений не осталось

        used[best.f][best.idx] = used[best.t][best.idx] = 1;
        m.portals.push_back({best.f, best.t, best.idx % W, best.idx / W});
    }

    // 3) Лишние спуски для интереса (связность уже есть).
    for (int k = 0; k < config.extra_portals; ++k) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (F < 2) break;
            const int f = rnd(rng, 1, F - 1);
            std::vector<int> targets;
            for (int t = f - 1; t >= 0; --t) {
                if (portal_allowed(f, t)) targets.push_back(t);
            }
            if (targets.empty()) continue;
            const int t = targets[rnd(rng, 0, static_cast<int>(targets.size()) - 1)];
            if (try_add_portal(f, t)) break;
        }
    }

    return m;
}

bool Maze_Generator::is_connected(const Maze& maze) {
    const int total = static_cast<int>(maze.floors.size()) * maze.W * maze.H;
    if (total == 0) return true;
    Dsu dsu(total);
    build_dsu(maze, dsu);
    const int root = dsu.find(0);
    for (int i = 1; i < total; ++i) {
        if (dsu.find(i) != root) return false;
    }
    return true;
}

Maze_Size Maze_Generator::size_of(const Maze& maze) {
    return {2 * maze.W + 1,
            static_cast<int>(maze.floors.size()) * k_floor_height + 1,
            2 * maze.H + 1};
}

void Maze_Generator::place(const Maze& maze, int ox, int oy, int oz, const Block_Setter& setter) {
    const Maze_Size size = size_of(maze);
    const int F = static_cast<int>(maze.floors.size());
    constexpr int k_wall_height = k_floor_height - 1;

    // 1) Очистка объёма.
    for (int y = 0; y < size.y; ++y)
        for (int z = 0; z < size.z; ++z)
            for (int x = 0; x < size.x; ++x)
                setter(ox + x, oy + y, oz + z, Block_Types::Air, 0);

    // Сетка блоков: клетки лабиринта — на нечётных координатах (2x+1, 2y+1), стены — между ними,
    // столбы — на чётных парах (2i, 2j).
    for (int f = 0; f < F; ++f) {
        const int base = oy + f * k_floor_height;   // перекрытие этажа f

        for (int z = 0; z < size.z; ++z)
            for (int x = 0; x < size.x; ++x)
                setter(ox + x, base, oz + z, Block_Types::Stone, 0);

        const Maze_Floor& fl = maze.floors[f];
        for (int y = 0; y < fl.H; ++y) {
            for (int x = 0; x < fl.W; ++x) {
                const uint8_t walls = fl.at(x, y).walls;
                const int gx = 2 * x + 1;
                const int gz = 2 * y + 1;
                auto wall_column = [&](int wx, int wz) {
                    for (int h = 1; h <= k_wall_height; ++h)
                        setter(ox + wx, base + h, oz + wz, Block_Types::Stone, 0);
                };
                if (walls & 0b0001) wall_column(gx, gz - 1);   // N
                if (walls & 0b0010) wall_column(gx + 1, gz);   // E
                if (walls & 0b0100) wall_column(gx, gz + 1);   // S
                if (walls & 0b1000) wall_column(gx - 1, gz);   // W
            }
        }

        // Столбы по углам сетки.
        for (int z = 0; z < size.z; z += 2)
            for (int x = 0; x < size.x; x += 2)
                for (int h = 1; h <= k_wall_height; ++h)
                    setter(ox + x, base + h, oz + z, Block_Types::Stone, 0);
    }

    // Крыша над верхним этажом.
    const int roof = oy + F * k_floor_height;
    for (int z = 0; z < size.z; ++z)
        for (int x = 0; x < size.x; ++x)
            setter(ox + x, roof, oz + z, Block_Types::Stone, 0);

    // 2) Спуски: ступеньки по кольцу 2x2 клеток (см. комментарий в Maze.h).
    //    Ступень k стоит в клетке ring[k % 4] на уровне L_k = верх_этажа_f - k: её высокая половина
    //    смотрит на предыдущую клетку кольца, поэтому от ступени к ступени спуск ровно на блок.
    //    Между соседними клетками кольца — камень на уровне следующей ступени (проход).
    //    Под всем этим — камень до нижней ступени кольца (не глубже: опоры не проходят через уровни
    //    других спусков); над ступенями и проходами — воздух, 3 блока.
    for (const Maze_Portal& p : maze.portals) {
        const int steps = k_floor_height * (p.from_floor - p.to_floor);
        const int sx = (p.x + 1 < maze.W) ? 1 : -1;
        const int sy = (p.y + 1 < maze.H) ? 1 : -1;
        const int ring_x[4] = {p.x, p.x + sx, p.x + sx, p.x};
        const int ring_y[4] = {p.y, p.y, p.y + sy, p.y + sy};
        const int top = oy + p.from_floor * k_floor_height;   // уровень перекрытия верхнего этажа
        auto level = [&](int k) { return top - k; };
        auto grid_x = [&](int cell_x) { return 2 * cell_x + 1; };
        auto grid_z = [&](int cell_y) { return 2 * cell_y + 1; };
        // Направление от клетки a к соседней b -> Block_Meta::facing_*.
        auto dir_meta = [](int ax, int ay, int bx, int by) -> uint8_t {
            const int dx = bx - ax;
            const int dy = by - ay;
            if (dy < 0) return Block_Meta::facing_north;
            if (dx > 0) return Block_Meta::facing_east;
            if (dy > 0) return Block_Meta::facing_south;
            return Block_Meta::facing_west;
        };

        // Опоры: камень от земли до ступени.
        for (int k = 0; k <= steps; ++k) {
            const int r = k % 4;
            const int gx = ox + grid_x(ring_x[r]);
            const int gz = oz + grid_z(ring_y[r]);
            for (int y = level(steps); y < level(k); ++y)
                setter(gx, y, gz, Block_Types::Stone, 0);
        }
        // Проходы между соседними клетками кольца (в позиции стены), уровень — следующей ступени.
        for (int k = 0; k < steps; ++k) {
            const int a = k % 4;
            const int b = (k + 1) % 4;
            const int gx = ox + (grid_x(ring_x[a]) + grid_x(ring_x[b])) / 2;
            const int gz = oz + (grid_z(ring_y[a]) + grid_z(ring_y[b])) / 2;
            for (int y = level(steps); y <= level(k + 1); ++y)
                setter(gx, y, gz, Block_Types::Stone, 0);
        }
        // Ступени.
        for (int k = 0; k <= steps; ++k) {
            const int r = k % 4;
            const int prev = (r + 3) % 4;
            setter(ox + grid_x(ring_x[r]), level(k), oz + grid_z(ring_y[r]), Block_Types::Stone_Stairs,
                   dir_meta(ring_x[r], ring_y[r], ring_x[prev], ring_y[prev]));
        }
        // Воздух над ступенями и проходами.
        for (int k = 0; k <= steps; ++k) {
            const int r = k % 4;
            for (int y = level(k) + 1; y <= level(k) + k_wall_height; ++y)
                setter(ox + grid_x(ring_x[r]), y, oz + grid_z(ring_y[r]), Block_Types::Air, 0);
        }
        for (int k = 0; k < steps; ++k) {
            const int a = k % 4;
            const int b = (k + 1) % 4;
            const int gx = ox + (grid_x(ring_x[a]) + grid_x(ring_x[b])) / 2;
            const int gz = oz + (grid_z(ring_y[a]) + grid_z(ring_y[b])) / 2;
            for (int y = level(k + 1) + 1; y <= level(k + 1) + k_wall_height; ++y)
                setter(gx, y, gz, Block_Types::Air, 0);
        }
    }
}
