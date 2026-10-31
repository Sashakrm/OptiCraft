#ifndef OPTICRAFT_DUNGEON_LAYOUT_H
#define OPTICRAFT_DUNGEON_LAYOUT_H

#include "world/blocks/Block_Types.h"

#include <cstdint>
#include <vector>

// Зачаток данжа: одна комната с каменными стенами, полом и потолком, один проём
// и сундук в углу. Раскладка детерминирована по seed — одинаковый seed даёт
// одинаковый данж. Генератор ничего не ставит в мир сам: он только выдаёт список
// блоков относительно точки отсчёта (origin), а размещение делает Game.
struct Dungeon_Block {
    int x = 0;
    int y = 0;
    int z = 0;
    Block_Types type = Block_Types::Air;
};

struct Dungeon_Layout {
    std::vector<Dungeon_Block> blocks;
    int size_x = 0;
    int size_y = 0;
    int size_z = 0;
};

Dungeon_Layout make_dungeon_layout(std::uint32_t seed);

#endif // OPTICRAFT_DUNGEON_LAYOUT_H
