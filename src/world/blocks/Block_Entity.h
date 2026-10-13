//
// Block_Entity — данные, которые живут "внутри" отдельного блока и не влезают в один
// байт Chunk_Block_Grid: содержимое сундука, сетка верстака, слоты печи. Сам блок
// (Block_Types::Chest/Crafting_Table/Furnace) — это только то, что видно в чанке и
// из чего строится меш; Block_Entity — это состояние ЭТОГО КОНКРЕТНОГО блока в мире,
// как инвентарь сундука в Minecraft.
//
// Хранится не в Chunk (одна запись на редкий "особый" блок, незачем раздувать каждый
// чанк), а плоской картой в Chunk_Manager: world position -> Block_Entity (см.
// Chunk_Manager::m_block_entities). Персистентность — отдельный JSON рядом с world.json
// (см. Block_Entity_Store), а не бинарный формат Region_File: тот жёстко заточен под
// ровно 32 КБ блоков на чанк, и на скорую руку пристёгивать к нему записи переменной
// длины — источник тонких багов в устойчивости к падению процесса, которой Region_File
// как раз старается добиться (см. комментарий в Region_File.h).
//
#ifndef OPTICRAFT_BLOCK_ENTITY_H
#define OPTICRAFT_BLOCK_ENTITY_H

#include <cstdint>
#include <string>
#include <vector>
#include "entities/player/Inventory.h" // Item_Stack — предметы в этой игре это блоки, отдельного реестра предметов нет

enum class Block_Entity_Type : uint8_t {
    None = 0,
    Chest = 1,
    Crafting_Table = 2,
    Furnace = 3
};

// Печь: 2 слота, которыми управляет игрок (вход сверху, топливо снизу — как в Minecraft),
// плюс 1 выходной. Жарка идёт в Chunk_Manager::update_block_entities по таблице
// assets/smelting.json (см. Smelting.h), состояние горения лежит в полях ниже.
namespace Block_Entity_Slots {
    constexpr size_t chest = 27;
    constexpr size_t crafting_table = 9; // сетка 3x3, выход не хранится — считается на лету по Recipe_Registry
    constexpr size_t furnace = 3;        // 0 = вход (верх), 1 = топливо (низ), 2 = выход

    constexpr size_t furnace_input = 0;
    constexpr size_t furnace_fuel = 1;
    constexpr size_t furnace_output = 2;
}

struct Block_Entity {
    Block_Entity_Type type = Block_Entity_Type::None;
    std::vector<Item_Stack> slots;

    // Двойной сундук: смещение до "второй половины" в мировых координатах (только X/Z,
    // сундуки не связываются по Y). (0,0) значит "не связан" — валидная ссылка обязана
    // быть ненулевой, потому что связывать сундук сам с собой бессмысленно.
    int8_t paired_dx = 0;
    int8_t paired_dz = 0;
    bool has_pair() const { return paired_dx != 0 || paired_dz != 0; }

    // Печь (секунды): сколько ещё горит текущая порция топлива / на сколько она была рассчитана
    // (для шкалы огня) / сколько уже прожарен предмет во входном слоте. Сохраняются на диск.
    float burn_time_left = 0.0f;
    float burn_time_total = 0.0f;
    float cook_progress = 0.0f;
    bool is_burning() const { return burn_time_left > 0.0f; }

    static Block_Entity make(Block_Entity_Type entity_type) {
        Block_Entity entity;
        entity.type = entity_type;
        switch (entity_type) {
            case Block_Entity_Type::Chest:
                entity.slots.assign(Block_Entity_Slots::chest, Item_Stack{});
                break;
            case Block_Entity_Type::Crafting_Table:
                entity.slots.assign(Block_Entity_Slots::crafting_table, Item_Stack{});
                break;
            case Block_Entity_Type::Furnace:
                entity.slots.assign(Block_Entity_Slots::furnace, Item_Stack{});
                break;
            case Block_Entity_Type::None:
                break;
        }
        return entity;
    }
};

// Есть ли у типа блока Block_Entity вообще (используется при постановке/поломке блока,
// см. Chunk_Manager::set_block_world) — единая точка, откуда это спрашивают, чтобы
// добавление нового "блока с начинкой" в будущем не потребовало искать все места вручную.
Block_Entity_Type block_entity_type_for(Block_Types type);

const char* block_entity_type_name(Block_Entity_Type type); // "chest"/"crafting_table"/"furnace" — для сериализации
Block_Entity_Type block_entity_type_from_name(const std::string& name);

#endif //OPTICRAFT_BLOCK_ENTITY_H
