//
// Created by noktemor on 30.03.2026.
//

#ifndef OPTICRAFT_NOISE_H
#define OPTICRAFT_NOISE_H



#include <array>
#include <cstdint>
#include <glm/glm.hpp>

class Perlin_Noise {
private:
    std::array<int, 256> m_permutation_table;

    // Параметры шума
    int m_seed;
    int m_octaves;
    float m_persistence;
    float m_lacunarity;

    // Внутренние утилиты
    static float fade(float t);
    static float lerp(float a, float b, float t);
    static float grad(int hash, float x, float y, float z);

    int hash(int x) const;
    float noise_3d(float x, float y, float z) const;

public:
    Perlin_Noise(int seed = 12345,
                 int octaves = 4,
                 float persistence = 0.5f,
                 float lacunarity = 2.0f);

    // 2D шум для высоты ландшафта
    float get_height_noise(float x, float z) const;

    // 3D шум для пещер/структур
    float get_cave_noise(float x, float y, float z) const;

    // Сеттеры для гибкой настройки
    void set_octaves(int octaves) { m_octaves = octaves; }
    void set_persistence(float p) { m_persistence = p; }
    void set_lacunarity(float l) { m_lacunarity = l; }
};


#endif //OPTICRAFT_NOISE_H
