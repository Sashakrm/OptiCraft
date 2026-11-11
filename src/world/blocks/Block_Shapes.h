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
int shape_boxes(Block_Shape shape, uint8_t meta, Shape_Box out[8]);

// Фигуры с точным хитбоксом (Button, Wire): луч прицела проходит мимо пустой части клетки, а подсветка
// рисуется по реальному боксу, а не по всей клетке.
inline bool shape_has_precise_hitbox(Block_Shape shape) {
    return shape == Block_Shape::Button || shape == Block_Shape::Wire ||
           shape == Block_Shape::Stairs;
}

// Направление "высокой" стороны ступенек для meta: 0..3 (как Block_Meta::facing_*).
uint8_t stairs_high_side(uint8_t meta);

// Противоположное направление (для установки: ступенька высокой стороной от игрока).
uint8_t opposite_facing(uint8_t meta);

// Вычисляет угол ступеньки по соседним ступенькам перед/за ней. Параметры содержат только
// наличие лестницы и её направление (bits 0..1); возвращает метаданные для shape_boxes().
uint8_t stairs_effective_meta(uint8_t meta, bool front_is_stairs, uint8_t front_meta,
                              bool back_is_stairs, uint8_t back_meta);

#endif //OPTICRAFT_BLOCK_SHAPES_H
