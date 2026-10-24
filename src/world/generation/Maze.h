#ifndef OPTICRAFT_MAZE_H
#define OPTICRAFT_MAZE_H

// Многоэтажный лабиринт (генератор структур). Не подключён к миру: только данные и
// функция, которая кладёт лабиринт в блоки через Block_Setter, как и остальные генераторы.
//
//  * Этажи: от minFloors до maxFloors. Площадь этажа в клетках — [min_area, max_area],
//    стороны независимы, соотношение сторон ограничено max_aspect.
//  * Каждый этаж — идеальный лабиринт (recursive backtracker), клетки связаны стенами.
//  * Спуски (порталы) между этажами: с этажа f на t = f-1 всегда; с f >= 5 разрешён
//    прыжок на f-2. Минимум спусков растёт с номером этажа, затем сшиваются компоненты
//    связности и добавляются лишние спуски.
//  * В блоках: стены и пол/потолки этажей — камень (Block_Types::Stone), воздух — внутри.
//  * Спуск — ступеньки (Block_Types::Stone_Stairs) по кольцу 2x2 клеток: на каждую клетку
//    кольца ступень опускается на один блок, между клетками — проход из камня. Кольцо
//    возвращается в клетку портала на нижнем этаже, поэтому связность совпадает с физикой.

#include <cstdint>
#include <functional>
#include <vector>
#include "math/Fast_Rng.h"
#include "world/blocks/Block_Types.h"

struct Maze_Config {
    int min_area = 70;
    int max_area = 250;
    int min_side = 5;
    int max_side = 50;
    int max_aspect = 4;        // max(W,H) / min(W,H)
    int min_floors = 3;
    int max_floors = 7;
    int extra_portals = 2;     // дополнительные спуски сверх нужных для связности
};

// Биты стен клетки: 0 = N, 1 = E, 2 = S, 3 = W. 1 — стена есть.
struct Maze_Cell {
    uint8_t walls = 0b1111;
};

struct Maze_Floor {
    int W = 0;
    int H = 0;
    std::vector<Maze_Cell> cells;

    Maze_Cell& at(int x, int y) { return cells[y * W + x]; }
    const Maze_Cell& at(int x, int y) const { return cells[y * W + x]; }
    bool can_go(int x, int y, int dir) const { return !(cells[y * W + x].walls & (1u << dir)); }
};

struct Maze_Portal {
    int from_floor = 0;        // верхний этаж
    int to_floor = 0;          // нижний этаж (to_floor < from_floor)
    int x = 0;
    int y = 0;
};

struct Maze {
    int W = 0;
    int H = 0;
    std::vector<Maze_Floor> floors;   // floors[0] — нижний
    std::vector<Maze_Portal> portals;
};

// Размер лабиринта в блоках: x = 2W+1, y = этажи * высота этажа + крыша, z = 2H+1.
struct Maze_Size {
    int x = 0;
    int y = 0;
    int z = 0;
};

class Maze_Generator {
public:
    // meta — для ступеней: направление высокой стороны (Block_Meta::facing_*), иначе 0.
    using Block_Setter = std::function<void(int x, int y, int z, Block_Types type, uint8_t meta)>;

    static constexpr int k_floor_height = 4;   // слой пола (1 блок) + воздух (3 блока)

    static Maze generate(const Maze_Config& config, Fast_Rng& rng);
    static bool is_connected(const Maze& maze);
    static Maze_Size size_of(const Maze& maze);

    // Ставит лабиринт, нижний угол которого в (ox, oy, oz). Очищает объём, затем кладёт камень.
    static void place(const Maze& maze, int ox, int oy, int oz, const Block_Setter& setter);
};

#endif //OPTICRAFT_MAZE_H
