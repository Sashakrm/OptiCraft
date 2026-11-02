#include "world/structures/Dungeon_Layout.h"

#include <random>

Dungeon_Layout make_dungeon_layout(std::uint32_t seed) {
    std::mt19937 rng(seed);
    auto roll = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    Dungeon_Layout layout;
    const int inner_x = roll(5, 9);   // внутренний размер комнаты
    const int inner_z = roll(5, 9);
    const int inner_y = roll(3, 4);
    // Снаружи — рамка толщиной 1 блок.
    layout.size_x = inner_x + 2;
    layout.size_y = inner_y + 2;
    layout.size_z = inner_z + 2;

    // Стены/пол/потолок: булыжник с редкими mossy-вставками, как у старых руин.
    auto stone_for = [&]() { return roll(0, 3) == 0 ? Block_Types::Mossy_Stone : Block_Types::Cobblestone; };

    // Проём в середине одной из четырёх стен.
    const int door_side = roll(0, 3);
    const int door_offset = roll(1, door_side < 2 ? inner_z : inner_x);

    for (int x = 0; x < layout.size_x; ++x) {
        for (int y = 0; y < layout.size_y; ++y) {
            for (int z = 0; z < layout.size_z; ++z) {
                const bool shell = x == 0 || y == 0 || z == 0 ||
                                   x == layout.size_x - 1 || y == layout.size_y - 1 || z == layout.size_z - 1;
                if (!shell) {
                    // Внутренность очищаем: на случай, если место заполнено землёй/камнем.
                    layout.blocks.push_back({x, y, z, Block_Types::Air});
                    continue;
                }
                Block_Types type = stone_for();
                // Проём: на стене door_side пропускаем блок на высоте 1..2 по центру-смещению.
                const bool on_door_wall =
                    (door_side == 0 && x == 0) || (door_side == 1 && x == layout.size_x - 1) ||
                    (door_side == 2 && z == 0) || (door_side == 3 && z == layout.size_z - 1);
                if (on_door_wall && y >= 1 && y <= 2) {
                    const int along = door_side < 2 ? z : x;
                    if (along == door_offset) type = Block_Types::Air;
                }
                layout.blocks.push_back({x, y, z, type});
            }
        }
    }

    // Сундук в углу внутри комнаты (блок-сущность появится при размещении через Game).
    layout.blocks.push_back({1, 1, 1, Block_Types::Chest});
    return layout;
}
