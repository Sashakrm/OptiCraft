//
// Created by noktemor on 30.03.2026.
//

#include "Biome_Types.h"
#include <algorithm>

Biome_Registry::Biome_Registry() {
    // Инициализация дефолтными значениями
    for (auto& props : m_properties) {
        props = {
            "Unknown",
            glm::vec3(0.5f), glm::vec3(0.5f), glm::vec3(0.5f), glm::vec3(0.5f),
            0.5f, 0.5f, 0.5f,
            0.01f, 50.0f, 0.5f,
            0.0f, 0.0f, 0.0f,
            Block_Type::Stone, Block_Type::Dirt, Block_Type::Stone, Block_Type::Sand,
            0.5f, 10.0f,
            128.0f, false, false
        };
    }

    // ✅ Регистрация базовых биомов (легко расширять до 30-50)

    // 🌊 Океан
    m_properties[static_cast<size_t>(Biome_Type::Ocean)] = {
        "Ocean",
        glm::vec3(0.4f, 0.6f, 0.8f),  // fog
        glm::vec3(0.2f, 0.4f, 0.7f),  // water
        glm::vec3(0.3f, 0.5f, 0.3f),  // grass
        glm::vec3(0.3f, 0.5f, 0.3f),  // foliage
        0.5f,   // temperature
        0.8f,   // humidity
        0.2f,   // elevation (низко)
        0.01f, 50.0f, 0.3f,
        0.0f, 0.0f, 0.0f,
        Block_Type::Sand, Block_Type::Dirt, Block_Type::Stone, Block_Type::Sand,
        0.3f, 15.0f,
        0.0f, false, true
    };

    // 🏖️ Пляж
    m_properties[static_cast<size_t>(Biome_Type::Beach)] = {
        "Beach",
        glm::vec3(0.7f, 0.7f, 0.8f),
        glm::vec3(0.3f, 0.5f, 0.7f),
        glm::vec3(0.4f, 0.6f, 0.4f),
        glm::vec3(0.4f, 0.6f, 0.4f),
        0.6f, 0.5f, 0.1f,
        0.02f, 30.0f, 0.5f,
        0.01f, 0.0f, 0.1f,
        Block_Type::Sand, Block_Type::Sand, Block_Type::Stone, Block_Type::Sand,
        0.2f, 8.0f,
        0.0f, false, false
    };

    // 🌿 Равнины
    m_properties[static_cast<size_t>(Biome_Type::Plains)] = {
        "Plains",
        glm::vec3(0.8f, 0.9f, 1.0f),
        glm::vec3(0.3f, 0.5f, 0.7f),
        glm::vec3(0.5f, 0.7f, 0.3f),
        glm::vec3(0.5f, 0.7f, 0.3f),
        0.6f, 0.4f, 0.3f,
        0.015f, 40.0f, 0.4f,
        0.05f, 0.1f, 0.3f,
        Block_Type::Grass, Block_Type::Dirt, Block_Type::Stone, Block_Type::Dirt,
        0.4f, 12.0f,
        180.0f, false, false
    };

    // 🌲 Лес
    m_properties[static_cast<size_t>(Biome_Type::Forest)] = {
        "Forest",
        glm::vec3(0.7f, 0.8f, 0.9f),
        glm::vec3(0.3f, 0.5f, 0.7f),
        glm::vec3(0.4f, 0.6f, 0.2f),
        glm::vec3(0.3f, 0.5f, 0.2f),
        0.5f, 0.7f, 0.4f,
        0.012f, 60.0f, 0.6f,
        0.3f, 0.2f, 0.4f,
        Block_Type::Grass, Block_Type::Dirt, Block_Type::Stone, Block_Type::Dirt,
        0.5f, 10.0f,
        150.0f, false, false
    };

    // 🏔️ Горы
    m_properties[static_cast<size_t>(Biome_Type::Mountains)] = {
        "Mountains",
        glm::vec3(0.9f, 0.9f, 1.0f),
        glm::vec3(0.3f, 0.5f, 0.7f),
        glm::vec3(0.5f, 0.6f, 0.4f),
        glm::vec3(0.4f, 0.5f, 0.3f),
        0.4f, 0.3f, 0.9f,  // Высокая elevation
        0.008f, 120.0f, 0.8f,
        0.1f, 0.05f, 0.2f,
        Block_Type::Stone, Block_Type::Stone, Block_Type::Stone, Block_Type::Stone,
        0.6f, 20.0f,
        200.0f, true, false
    };

    // ❄️ Тундра
    m_properties[static_cast<size_t>(Biome_Type::Tundra)] = {
        "Tundra",
        glm::vec3(0.9f, 0.9f, 1.0f),
        glm::vec3(0.4f, 0.6f, 0.8f),
        glm::vec3(0.7f, 0.8f, 0.7f),
        glm::vec3(0.6f, 0.7f, 0.6f),
        0.1f,   // Холодно
        0.3f,
        0.4f,
        0.02f, 50.0f, 0.5f,
        0.02f, 0.05f, 0.2f,
        Block_Type::Grass, Block_Type::Dirt, Block_Type::Stone, Block_Type::Dirt,
        0.3f, 10.0f,
        80.0f, true, true  // Снег и лёд
    };

    // 🏜️ Пустыня
    m_properties[static_cast<size_t>(Biome_Type::Desert)] = {
        "Desert",
        glm::vec3(1.0f, 0.95f, 0.8f),
        glm::vec3(0.5f, 0.6f, 0.7f),
        glm::vec3(0.8f, 0.8f, 0.6f),
        glm::vec3(0.7f, 0.7f, 0.5f),
        0.95f,  // Жарко
        0.05f,  // Сухо
        0.3f,
        0.02f, 40.0f, 0.3f,
        0.0f, 0.0f, 0.0f,
        Block_Type::Sand, Block_Type::Sand, Block_Type::Stone, Block_Type::Sand,
        0.2f, 15.0f,
        256.0f, false, false
    };

    // 🌴 Джунгли
    m_properties[static_cast<size_t>(Biome_Type::Jungle)] = {
        "Jungle",
        glm::vec3(0.7f, 0.8f, 0.7f),
        glm::vec3(0.3f, 0.5f, 0.6f),
        glm::vec3(0.3f, 0.6f, 0.2f),
        glm::vec3(0.2f, 0.5f, 0.1f),
        0.9f,   // Жарко
        0.95f,  // Очень влажно
        0.4f,
        0.01f, 80.0f, 0.7f,
        0.5f, 0.3f, 0.6f,
        Block_Type::Grass, Block_Type::Dirt, Block_Type::Stone, Block_Type::Dirt,
        0.4f, 8.0f,
        150.0f, false, false
    };

    // 🍄 Грибные поля
    m_properties[static_cast<size_t>(Biome_Type::Mushroom_Fields)] = {
        "Mushroom_Fields",
        glm::vec3(0.8f, 0.7f, 0.9f),
        glm::vec3(0.4f, 0.3f, 0.5f),
        glm::vec3(0.7f, 0.5f, 0.7f),
        glm::vec3(0.8f, 0.6f, 0.8f),
        0.5f, 0.9f, 0.2f,
        0.01f, 20.0f, 0.2f,
        0.0f, 0.4f, 0.5f,
        Block_Type::Mycelium, Block_Type::Dirt, Block_Type::Stone, Block_Type::Dirt,
        0.1f, 5.0f,
        128.0f, false, false
    };
}

Biome_Registry& Biome_Registry::get_instance() {
    static Biome_Registry instance;
    return instance;
}

const Biome_Properties& Biome_Registry::get(Biome_Type type) const {
    return m_properties[static_cast<size_t>(type)];
}

Biome_Type Biome_Registry::from_name(const std::string& name) const {
    for (size_t i = 0; i < static_cast<size_t>(Biome_Type::Count); ++i) {
        if (m_properties[i].name == name) {
            return static_cast<Biome_Type>(i);
        }
    }
    return Biome_Type::Plains;  // Default
}

void Biome_Registry::register_biome(Biome_Type type, const Biome_Properties& props) {
    m_properties[static_cast<size_t>(type)] = props;
}
