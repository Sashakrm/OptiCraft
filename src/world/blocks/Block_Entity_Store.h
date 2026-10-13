//
// Block_Entity_Store — сохранение/загрузка содержимого блоков-контейнеров (см.
// Block_Entity.h) в отдельный JSON-файл рядом с world.json. Намеренно НЕ трогает
// формат Region_File (см. комментарий в Block_Entity.h почему) — блоков с начинкой
// на весь мир единицы по сравнению с количеством обычных блоков, так что простой
// JSON "весь список сразу" читается/пишется мгновенно и не нуждается в поблочной
// организации, как у Region_File.
//
#ifndef OPTICRAFT_BLOCK_ENTITY_STORE_H
#define OPTICRAFT_BLOCK_ENTITY_STORE_H

#include <filesystem>
#include <unordered_map>
#include <glm/glm.hpp>
#include "world/blocks/Block_Entity.h"
#include "math/Hash_Utils.h"

using Block_Entity_Map = std::unordered_map<glm::ivec3, Block_Entity, Chunk_Key_Hash>;

namespace Block_Entity_Store {
    // false, если файла нет (новый мир/пресет генерации — не ошибка) или он битый
    // (лог пишет сам, вызывающий код просто получает пустую карту и продолжает).
    bool load(const std::filesystem::path& path, Block_Entity_Map& out_entities);
    bool save(const std::filesystem::path& path, const Block_Entity_Map& entities);
}

#endif //OPTICRAFT_BLOCK_ENTITY_STORE_H
