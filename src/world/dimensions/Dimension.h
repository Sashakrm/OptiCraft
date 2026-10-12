//
// Dimension — описание измерения мира и режима генерации его рельефа.
//
// Как добавить новое измерение (например, Nether):
//   1. Добавить имя в enum Dimension_Id (перед Count).
//   2. Добавить одну запись в таблицу в Dimension.cpp (register_dimensions): имя, режим рельефа
//      (Terrain_Mode) и параметры. Для Terrain_Mode::Noise берутся обычные шумы World_Generator,
//      для Flat — список слоёв, для Void — пустота.
//   3. Если нужен совсем новый алгоритм рельефа — добавить значение в Terrain_Mode и ветку в
//      World_Generator (get_block / generate_chunk / chunk_is_definitely_air); все они уже
//      делают switch по Terrain_Mode, так что компилятор подскажет, где что дописать.
// Перемещения между измерениями здесь НЕТ — это только основа: данные + генерация + хранение.
//
#ifndef OPTICRAFT_DIMENSION_H
#define OPTICRAFT_DIMENSION_H

#include <cstdint>
#include <string>
#include <vector>
#include "world/blocks/Block_Types.h"

enum class Dimension_Id : uint8_t {
    Overworld = 0,
    Flat_Test,     // плоский мир со слоями — пример режима Flat, годится для тестов построек
    Void_Test,     // пустота — пример режима Void
    Count
};

// Как генерируется рельеф измерения.
enum class Terrain_Mode : uint8_t {
    Noise = 0,     // обычный мир: климат, биомы, пещеры, острова (World_Generator)
    Flat,          // плоские слои из Dimension_Info::flat_layers
    Void           // пустота (всё — воздух)
};

// Один слой плоского мира; слои перечисляются СНИЗУ ВВЕРХ начиная с flat_base_y.
struct Flat_Layer {
    Block_Types block = Block_Types::Stone;
    int thickness = 1;
};

struct Dimension_Info {
    Dimension_Id id = Dimension_Id::Overworld;
    const char* name = "overworld";        // ключ для world.json и имён файлов
    const char* display_name = "Overworld";
    Terrain_Mode terrain = Terrain_Mode::Noise;

    // Параметры, которые в будущем смогут отличаться у измерений.
    bool decorations = true;               // деревья, трава, цветы (decorate_column)
    bool has_sky_light = true;             // нет неба (пещерное измерение) — false
    bool sky_islands = true;               // парящие острова (только Noise)

    // Flat: слои и нижняя отметка. Void: spawn_height — куда ставить игрока.
    std::vector<Flat_Layer> flat_layers;
    int flat_base_y = 0;
    int spawn_height = 64;

    // Верхний Y последнего слоя плоского мира (включительно); для Noise/Void — flat_base_y - 1.
    int flat_top_y() const;
    // Блок плоского мира на высоте y (Air выше слоёв и ниже flat_base_y).
    Block_Types flat_block_at(int y) const;

    // Ключ хранения чанков. Overworld сохраняет прежнее имя папки генерации (старые миры
    // читаются без изменений), остальные измерения получают отдельный префикс.
    std::string storage_key(const std::string& generation_folder) const;
};

const Dimension_Info& get_dimension_info(Dimension_Id id);
// По имени из world.json; неизвестное имя → Overworld.
Dimension_Id dimension_from_name(const std::string& name);

#endif // OPTICRAFT_DIMENSION_H
