//
// Created by noktemor on 30.03.2026.
//

#ifndef OPTICRAFT_BIOME_GENERATOR_H
#define OPTICRAFT_BIOME_GENERATOR_H



#include "Biome_Types.h"
#include "utils/Noise.h"
#include "utils/Config.h"

class Biome_Generator {
private:
    // ✅ Мульти-шум система для классификации биомов
    Perlin_Noise m_temperature_noise;
    Perlin_Noise m_humidity_noise;
    Perlin_Noise m_elevation_noise;
    Perlin_Noise m_continental_noise;  // Для крупных регионов

    int m_seed;

    // Кэширование (оптимизация для 30+ чанков)
    mutable Biome_Type m_last_biome;
    mutable int m_last_x;
    mutable int m_last_z;

public:
    Biome_Generator(int seed = Config::world_seed);

    // ✅ Получить биом по координатам
    Biome_Type get_biome(int world_x, int world_z) const;

    // ✅ Получить свойства (с кэшем)
    const Biome_Properties& get_properties(int world_x, int world_z) const;

    // 🔧 Для отладки: экспорт карты биомов
    void export_biome_map(const std::string& filename, int size_px) const;

    // 🔧 Для расширения: добавить новый параметр шума
    void add_custom_noise(const std::string& name, Perlin_Noise&& noise);

    Biome_Type classify_biome(
    float continental, float temperature, float humidity, float elevation
) const;
};



#endif //OPTICRAFT_BIOME_GENERATOR_H
