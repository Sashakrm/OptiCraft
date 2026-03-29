//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_BLOCK_TYPES_H
#define OPTICRAFT_BLOCK_TYPES_H

#include <string>
#include <cstdint>
#include <glm/glm.hpp>

using Block_ID = uint16_t;

enum class Block_Type : Block_ID {
    Air = 0,
    Grass = 1,
    Dirt = 2,
    Stone = 3,
    Count
};

struct Block_Properties {
    std::string name;
    bool is_solid;
    bool is_transparent;
    float hardness;

    // 🔹 Текстура: индекс тайла в атласе
    int texture_index_top;    // Для верхней грани (трава)
    int texture_index_side;   // Для боковых граней
    int texture_index_bottom; // Для нижней грани (корень травы)

    // 🔹 Или одно значение для всех граней:
    int get_texture_index(int face_direction) const {
        if (face_direction == 1) return texture_index_top;    // +Y
        if (face_direction == -1) return texture_index_bottom; // -Y
        return texture_index_side;  // боковые
    }
};

class Block_Registry {
private:
    Block_Properties m_properties[static_cast<size_t>(Block_Type::Count)];

    Block_Registry();

public:
    static Block_Registry& get_instance();

    const Block_Properties& get_properties(Block_Type type) const;
    Block_ID get_block_id(const std::string& name) const;
};

inline const Block_Properties& get_block_props(Block_Type type) {
    return Block_Registry::get_instance().get_properties(type);
}

#endif //OPTICRAFT_BLOCK_TYPES_H
