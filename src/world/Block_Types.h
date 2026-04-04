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
    Wood = 4,
    Leaves = 5,
    Sand = 6,
    Water = 7,
    Snow = 8,
    Mycelium = 9,
    Count
};

struct Block_Properties {
    std::string name;
    bool is_solid;
    bool is_transparent;
    float hardness;

    // Текстуры: имена файлов (без .png)
    std::string texture_side;
    std::string texture_top;
    std::string texture_bottom;

    // OpenGL ID (заполняется при загрузке в Renderer)
    unsigned int tex_id_side;
    unsigned int tex_id_top;
    unsigned int tex_id_bottom;

    Block_Properties(
        std::string n = "",
        bool solid = true,
        bool transp = false,
        float hard = 1.0f,
        std::string side = "",
        std::string top = "",
        std::string bottom = "",
        unsigned int id_side = 0,
        unsigned int id_top = 0,
        unsigned int id_bottom = 0
    ) : name(std::move(n)),
        is_solid(solid),
        is_transparent(transp),
        hardness(hard),
        texture_side(std::move(side)),
        texture_top(std::move(top)),
        texture_bottom(std::move(bottom)),
        tex_id_side(id_side),
        tex_id_top(id_top),
        tex_id_bottom(id_bottom)
    {}
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
