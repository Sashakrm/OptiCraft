//
// Created by noktemor on 28.03.2026.
//

#include "Block_Types.h"

Block_Registry::Block_Registry() {
    // Индексы тайлов в атласе (пример для 16x16 атласа):
    // 0=воздух, 1=трава-верх, 2=трава-бок, 3=трава-низ, 4=земля, 5=камень...

    m_properties[static_cast<size_t>(Block_Type::Air)] = {
        "Air", false, false, 0.0f,
        -1, -1, -1  // нет текстуры
    };

    m_properties[static_cast<size_t>(Block_Type::Grass)] = {
        "Grass", true, false, 1.0f,
        602,  // top: трава
        538,  // side: земля с травой по краю
        4   // bottom: земля
    };

    m_properties[static_cast<size_t>(Block_Type::Dirt)] = {
        "Dirt", true, false, 1.0f,
        4, 4, 4  // земля со всех сторон
    };

    m_properties[static_cast<size_t>(Block_Type::Stone)] = {
        "Stone", true, false, 2.0f,
        5, 5, 5  // камень со всех сторон
    };
}

Block_Registry& Block_Registry::get_instance() {
    static Block_Registry instance;
    return instance;
}

const Block_Properties& Block_Registry::get_properties(Block_Type type) const {
    return m_properties[static_cast<size_t>(type)];
}

Block_ID Block_Registry::get_block_id(const std::string& name) const {
    if (name == "Air") return static_cast<Block_ID>(Block_Type::Air);
    if (name == "Grass") return static_cast<Block_ID>(Block_Type::Grass);
    if (name == "Dirt") return static_cast<Block_ID>(Block_Type::Dirt);
    if (name == "Stone") return static_cast<Block_ID>(Block_Type::Stone);
    return 0;
}