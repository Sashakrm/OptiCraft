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
};

#endif //OPTICRAFT_HASH_UTILS_H
