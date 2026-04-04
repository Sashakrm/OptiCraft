//
// Created by noktemor on 28.03.2026.
//

#include "Block_Types.h"

Block_Registry::Block_Registry() {
    // === 🟤 Dirt (все грани одинаковые) ===
    m_properties[static_cast<size_t>(Block_Type::Dirt)] = {
        "Dirt", true, false, 1.0f,
        "dirt", "dirt", "dirt",
        0, 0, 0
    };

    // === Stone (все грани одинаковые) ===
    m_properties[static_cast<size_t>(Block_Type::Stone)] = {
        "Stone", true, false, 2.0f,
        "stone", "stone", "stone",
        0, 0, 0
    };

    // === 🏖️ Sand (все грани одинаковые) ===
    m_properties[static_cast<size_t>(Block_Type::Sand)] = {
        "Sand", true, false, 1.0f,
        "sand", "sand", "sand",
        0, 0, 0
    };

    // === 🍃 Leaves (прозрачная, все грани одинаковые) ===
    m_properties[static_cast<size_t>(Block_Type::Leaves)] = {
        "Leaves", true, true, 0.5f,  // ✅ transparent = true
        "leaves", "leaves", "leaves",
        0, 0, 0
    };

    // === 💧 Water (прозрачная, все грани одинаковые) ===
    m_properties[static_cast<size_t>(Block_Type::Water)] = {
        "Water", false, true, 100.0f,  // ✅ не solid, прозрачная
        "water", "water", "water",
        0, 0, 0
    };

    // === ❄️ Snow (все грани одинаковые) ===
    m_properties[static_cast<size_t>(Block_Type::Snow)] = {
        "Snow", true, false, 1.0f,
        "snow", "snow", "snow",
        0, 0, 0
    };

    // === 🌿 Grass (разные грани!) ===
    m_properties[static_cast<size_t>(Block_Type::Grass)] = {
        "Grass", true, false, 1.0f,
        "grass_side",  // side
        "grass_top",   // top
        "dirt",        // bottom (реюз текстуры земли)
        0, 0, 0
    };

    // === 🪵 Wood (верх/низ = кольца, бок = кора) ===
    m_properties[static_cast<size_t>(Block_Type::Wood)] = {
        "Wood", true, false, 2.0f,
        "wood_side",   // side (кора)
        "wood_top",    // top (кольца)
        "wood_top",    // bottom (кольца)
        0, 0, 0
    };

    // === 🟣 Mycelium (верх фиолетовый, бок серый, низ земля) ===
    m_properties[static_cast<size_t>(Block_Type::Mycelium)] = {
        "Mycelium", true, false, 1.0f,
        "mycelium_side",  // side
        "mycelium_top",   // top
        "dirt",           // bottom (реюз текстуры земли)
        0, 0, 0
    };

    // === ⚪ Air (пустота, нет текстур) ===
    m_properties[static_cast<size_t>(Block_Type::Air)] = {
        "Air", false, false, 0.0f,
        "", "", "",
        0, 0, 0
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
};