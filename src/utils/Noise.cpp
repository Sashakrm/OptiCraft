//
// Created by noktemor on 30.03.2026.
//

#include "Noise.h"
#include <cmath>
#include <random>
#include <algorithm>

// === Конструктор ===
Perlin_Noise::Perlin_Noise(int seed, int octaves, float persistence, float lacunarity)
    : m_seed(seed)
    , m_octaves(octaves)
    , m_persistence(persistence)
    , m_lacunarity(lacunarity)
{
    // Инициализация таблицы перестановок
    std::array<int, 256> indices;
    for (int i = 0; i < 256; ++i) indices[i] = i;

    // Перемешивание по сиду
    std::mt19937 rng(static_cast<unsigned>(seed));
    std::shuffle(indices.begin(), indices.end(), rng);

    for (int i = 0; i < 256; ++i) {
        m_permutation_table[i] = indices[i];
    }
}

// === Вспомогательные функции ===
float Perlin_Noise::fade(float t) {
    // Плавная функция интерполяции (3t² - 2t³)
    return t * t * (3.0f - 2.0f * t);
}

float Perlin_Noise::lerp(float a, float b, float t) {
    return a + t * (b - a);
}

float Perlin_Noise::grad(int hash, float x, float y, float z) {
    int h = hash & 15;
    float u = (h < 8) ? x : y;
    float v = (h < 4) ? y : ((h == 12 || h == 14) ? x : z);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

int Perlin_Noise::hash(int x) const {
    return m_permutation_table[x & 255];
}

// === Базовый 3D шум (октава 0) ===
float Perlin_Noise::noise_3d(float x, float y, float z) const {
    int x_int = static_cast<int>(std::floor(x)) & 255;
    int y_int = static_cast<int>(std::floor(y)) & 255;
    int z_int = static_cast<int>(std::floor(z)) & 255;

    float x_frac = x - std::floor(x);
    float y_frac = y - std::floor(y);
    float z_frac = z - std::floor(z);

    float u = fade(x_frac);
    float v = fade(y_frac);
    float w = fade(z_frac);

    int aa = hash(hash(x_int) + y_int) + z_int;
    int ab = hash(hash(x_int) + y_int) + z_int + 1;
    int ba = hash(hash(x_int + 1) + y_int) + z_int;
    int bb = hash(hash(x_int + 1) + y_int) + z_int + 1;

    float x1 = lerp(grad(hash(aa), x_frac, y_frac, z_frac),
                    grad(hash(ba), x_frac - 1, y_frac, z_frac), u);
    float x2 = lerp(grad(hash(ab), x_frac, y_frac - 1, z_frac),
                    grad(hash(bb), x_frac - 1, y_frac - 1, z_frac), u);
    float y1 = lerp(x1, x2, v);

    float x3 = lerp(grad(hash(aa + 1), x_frac, y_frac, z_frac - 1),
                    grad(hash(ba + 1), x_frac - 1, y_frac, z_frac - 1), u);
    float x4 = lerp(grad(hash(ab + 1), x_frac, y_frac - 1, z_frac - 1),
                    grad(hash(bb + 1), x_frac - 1, y_frac - 1, z_frac - 1), u);
    float y2 = lerp(x3, x4, v);

    return lerp(y1, y2, w) * 0.5f + 0.5f; // Нормализация в [0, 1]
}

// === Публичные методы ===
float Perlin_Noise::get_height_noise(float x, float z) const {
    float total = 0.0f;
    float frequency = 1.0f;
    float amplitude = 1.0f;
    float max_value = 0.0f;

    for (int i = 0; i < m_octaves; ++i) {
        total += noise_3d(x * frequency, 0, z * frequency) * amplitude;
        max_value += amplitude;
        amplitude *= m_persistence;
        frequency *= m_lacunarity;
    }

    return total / max_value; // Нормализация
}

float Perlin_Noise::get_cave_noise(float x, float y, float z) const {
    // Для пещер используем другой набор параметров
    float total = 0.0f;
    float frequency = 0.05f;  // Крупные пещеры
    float amplitude = 1.0f;

    for (int i = 0; i < 3; ++i) {  // Меньше октав для пещер
        total += noise_3d(x * frequency, y * frequency, z * frequency) * amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }

    return total;
}
