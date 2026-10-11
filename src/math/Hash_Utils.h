//
// Created by noktemor on 29.03.2026.
//

#ifndef OPTICRAFT_HASH_UTILS_H
#define OPTICRAFT_HASH_UTILS_H

#include <glm/glm.hpp>
#include <cstddef>

//  Хэш для glm::ivec2 (используется в Chunk_Manager и Renderer)
struct Chunk_Key_Hash {
    std::size_t operator()(const glm::ivec2& key) const {
        return std::hash<int>()(key.x) ^ (std::hash<int>()(key.y) << 1);
    }

    // Чанки теперь стекуются и по Y (см. Config::world_height_chunks), поэтому ключ
    // Chunk_Manager/Renderer — glm::ivec3 (chunk_x, chunk_y, chunk_z).
    std::size_t operator()(const glm::ivec3& key) const {
        std::size_t h = std::hash<int>()(key.x);
        h ^= std::hash<int>()(key.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(key.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

#endif //OPTICRAFT_HASH_UTILS_H
