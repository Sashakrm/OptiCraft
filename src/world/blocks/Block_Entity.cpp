#include "world/blocks/Block_Entity.h"
#include "world/blocks/Block_Types.h"

Block_Entity_Type block_entity_type_for(Block_Types type) {
    switch (type) {
        case Block_Types::Chest:          return Block_Entity_Type::Chest;
        case Block_Types::Crafting_Table: return Block_Entity_Type::Crafting_Table;
        case Block_Types::Furnace:        return Block_Entity_Type::Furnace;
        default:                          return Block_Entity_Type::None;
    }
}

const char* block_entity_type_name(Block_Entity_Type type) {
    switch (type) {
        case Block_Entity_Type::Chest:          return "chest";
        case Block_Entity_Type::Crafting_Table: return "crafting_table";
        case Block_Entity_Type::Furnace:        return "furnace";
        case Block_Entity_Type::None:            return "none";
    }
    return "none";
}

Block_Entity_Type block_entity_type_from_name(const std::string& name) {
    if (name == "chest") return Block_Entity_Type::Chest;
    if (name == "crafting_table") return Block_Entity_Type::Crafting_Table;
    if (name == "furnace") return Block_Entity_Type::Furnace;
    return Block_Entity_Type::None;
}
