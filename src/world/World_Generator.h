//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_WORLD_GENERATOR_H
#define OPTICRAFT_WORLD_GENERATOR_H

class Chunk;

#include "Block_Types.h"
#include "Biome_Generator.h"
#include "utils/Noise.h"
#include "utils/Config.h"
#include <random>
#include <glm/glm.hpp>

class World_Generator {
private:
    int m_seed;

    // 🎲 Генератор случайных чисел для структур
    std::mt19937 m_rng;

    // 🔊 Шумы для генерации
    Perlin_Noise m_terrain_noise;      // Основной рельеф
    Perlin_Noise m_detail_noise;       // Детали поверхности
    Perlin_Noise m_cave_noise;         // Пещеры (3D)
    Perlin_Noise m_ore_noise;          // Распределение руд

    // 🗺️ Генератор биомов
    Biome_Generator m_biome_gen;

    // ⚙️ Параметры
    float m_terrain_scale;
    float m_terrain_amplitude;

    // 🔧 Внутренние утилиты
    int world_to_local_y(int world_y) const;
    int local_to_world_y(int local_y, int chunk_base_y) const;

    // Генерация структур
    void generate_tree(Chunk* chunk, int local_x, int local_z, int surface_y, Biome_Type biome);
    void generate_ore_vein(Chunk* chunk, int local_x, int local_y, int local_z,
                          Block_Type ore_type, int size, float chance);

public:
    World_Generator(int seed = Config::world_seed);

    // ✅ Основная функция: заполнить чанк блоками
    void generate_chunk(Chunk* chunk);

    // ✅ Получить высоту поверхности в мировой точке (для отладки/интеграции)
    int get_surface_height(int world_x, int world_z) const;

    // ✅ Проверка: должен ли блок быть пещерой
    bool is_cave(int world_x, int world_y, int world_z) const;

    Block_Type determine_block(
    int wx, int wy, int wz,
    int surface_y, Biome_Type biome, const Biome_Properties& props
);

    // 🔧 Сеттеры для гибкой настройки
    void set_terrain_params(float scale, float amplitude);
    void set_cave_density(float density);
};

#endif //OPTICRAFT_WORLD_GENERATOR_H
