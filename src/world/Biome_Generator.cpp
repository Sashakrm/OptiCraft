//
// Created by noktemor on 30.03.2026.
//

// src/world/Biome_Generator.cpp
#include "Biome_Generator.h"
#include "Biome_Types.h"
#include <fstream>
#include <cmath>

Biome_Generator::Biome_Generator(int seed)
    : m_seed(seed)
    , m_temperature_noise(seed, 4, 0.5f, 2.0f)
    , m_humidity_noise(seed + 1000, 4, 0.5f, 2.0f)  // +1000 для уникальности
    , m_elevation_noise(seed + 2000, 5, 0.6f, 2.5f)
    , m_continental_noise(seed + 3000, 3, 0.4f, 3.0f)
    , m_last_biome(Biome_Type::Plains)
    , m_last_x(-999999)
    , m_last_z(-999999)
{}

Biome_Type Biome_Generator::get_biome(int world_x, int world_z) const {
    // ✅ Кэш для производительности (соседние блоки = тот же биом)
    if (world_x == m_last_x && world_z == m_last_z) {
        return m_last_biome;
    }

    // 1. Получаем параметры (масштабированные для крупных регионов)
    float continental = m_continental_noise.get_height_noise(
        world_x * 0.001f, world_z * 0.001f
    );  // [0..1] - материки/океаны

    float temperature = m_temperature_noise.get_height_noise(
        world_x * 0.002f + 5000, world_z * 0.002f + 5000
    );  // [0..1]

    float humidity = m_humidity_noise.get_height_noise(
        world_x * 0.002f + 10000, world_z * 0.002f + 10000
    );  // [0..1]

    float elevation = m_elevation_noise.get_height_noise(
        world_x * 0.003f, world_z * 0.003f
    );  // [0..1]

    // 2. Классификация биомов (✅ легко расширять до 30-50)
    Biome_Type biome = classify_biome(continental, temperature, humidity, elevation);

    // 3. Кэшируем
    m_last_biome = biome;
    m_last_x = world_x;
    m_last_z = world_z;

    return biome;
}

// ✅ Отдельная функция классификации — легко модифицировать
Biome_Type Biome_Generator::classify_biome(
    float continental, float temperature, float humidity, float elevation
) const {
    // 🌊 Океаны (низкая continental)
    if (continental < 0.3f) {
        if (elevation < 0.2f) return Biome_Type::Deep_Ocean;
        return Biome_Type::Ocean;
    }

    // 🏖️ Берега (переход вода/земля)
    if (continental < 0.35f || elevation < 0.15f) {
        return Biome_Type::Beach;
    }

    // ❄️ Холодные биомы
    if (temperature < 0.2f) {
        if (humidity > 0.7f) return Biome_Type::Ice_Spikes;
        if (elevation > 0.7f) return Biome_Type::Snowy_Mountains;
        return Biome_Type::Tundra;
    }

    // 🏜️ Сухие биомы
    if (humidity < 0.2f) {
        if (temperature > 0.8f) return Biome_Type::Desert;
        if (elevation > 0.6f) return Biome_Type::Badlands;
        return Biome_Type::Savanna;
    }

    // 🏔️ Горы (высокая elevation)
    if (elevation > 0.75f) {
        return Biome_Type::Mountains;
    }

    // 🌲 Леса (высокая влажность)
    if (humidity > 0.75f) {
        if (temperature > 0.85f) return Biome_Type::Jungle;
        if (temperature < 0.4f) return Biome_Type::Birch_Forest;
        return Biome_Type::Forest;
    }

    // 🌿 Равнины (умеренные параметры)
    if (humidity > 0.5f && temperature > 0.5f) {
        return Biome_Type::Flower_Forest;
    }

    // По умолчанию
    return Biome_Type::Plains;
}

const Biome_Properties& Biome_Generator::get_properties(int world_x, int world_z) const {
    Biome_Type biome = get_biome(world_x, world_z);
    return get_biome_props(biome);
}

void Biome_Generator::export_biome_map(const std::string& filename, int size_px) const {
    std::ofstream file(filename);
    if (!file.is_open()) return;

    file << "P6\n" << size_px << " " << size_px << "\n255\n";

    for (int z = 0; z < size_px; ++z) {
        for (int x = 0; x < size_px; ++x) {
            Biome_Type biome = get_biome(x * 10, z * 10);  // 10 блоков на пиксель
            const auto& props = get_biome_props(biome);

            // Записываем цвет биома
            uint8_t r = static_cast<uint8_t>(props.grass_color.r * 255);
            uint8_t g = static_cast<uint8_t>(props.grass_color.g * 255);
            uint8_t b = static_cast<uint8_t>(props.grass_color.b * 255);

            file << static_cast<char>(r) << static_cast<char>(g) << static_cast<char>(b);
        }
    }

    file.close();
}

void Biome_Generator::add_custom_noise(const std::string& name, Perlin_Noise&& noise) {
    // ✅ Для расширения: можно добавить 4-й, 5-й параметр шума
    // Например: m_river_noise, m_cave_density_noise, etc.
}