#ifndef OPTICRAFT_BLOCK_SHAPES_H
#define OPTICRAFT_BLOCK_SHAPES_H

// Геометрия блоков не-кубической формы (Block_Mesh_Type::Shaped). Блок — набор боксов в
// координатах внутри клетки [0,1]^3. Используется и мешером (отрисовка), и коллизией игрока,
// поэтому они всегда совпадают.
//
//  * Slab: нижняя половина клетки.
//  * Stairs: нижняя половина на всю клетку и верхняя половина со стороны, куда направлен meta
//    (Block_Meta::facing_*: -Z, +X, +Z, -X). Ступенька: со стороны "высокой" половины спуск на
//    полблока, дальше ещё полблока — за шаг из двух половин поднимается ровно один блок.

#include <cstdint>
#include <glm/glm.hpp>
#include "world/blocks/Block_Types.h"

struct Shape_Box {
    glm::vec3 lo{0.0f};
    glm::vec3 hi{1.0f};
};

// Заполняет out (до 2 боксов) и возвращает их число. Для Shape::None возвращает 0.
int shape_boxes(Block_Shape shape, uint8_t meta, Shape_Box out[2]);

// Направление "высокой" стороны ступенек для meta: 0..3 (как Block_Meta::facing_*).
uint8_t stairs_high_side(uint8_t meta);

// Противоположное направление (для установки: ступенька высокой стороной от игрока).
uint8_t opposite_facing(uint8_t meta);

#endif //OPTICRAFT_BLOCK_SHAPES_H
