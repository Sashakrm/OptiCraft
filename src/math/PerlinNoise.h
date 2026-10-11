//
// Created by noktemor on 27.04.2026.
//

#ifndef OPTICRAFT_PERLINNOISE_H
#define OPTICRAFT_PERLINNOISE_H

#include <vector>
#include <cmath>
#include <cstdint>
#include <random>
#include <utils/Config.h>

class PerlinNoise {
public:
    explicit PerlinNoise(std::uint32_t seed = Config::seed) {
        std::mt19937 gen(seed);
        std::uniform_int_distribution<uint8_t> dist;

        for (int i = 0; i < 256; i++)
            permutation[i] = i;

        // Перемешиваем
        for (int i = 255; i > 0; i--) {
            std::uniform_int_distribution<int> d(0, i);
            int j = d(gen);
            std::swap(permutation[i], permutation[j]);
            permutation[i + 256] = permutation[i];
        }
    }

    // 2D шум (для высоты ландшафта)
    double noise2D(double x, double y) const {
        int xi = static_cast<int>(std::floor(x)) & 255;
        int yi = static_cast<int>(std::floor(y)) & 255;

        double xf = x - std::floor(x);
        double yf = y - std::floor(y);

        double u = fade(xf);
        double v = fade(yf);

        int aa = permutation[permutation[xi] + yi];
        int ab = permutation[permutation[xi] + yi + 1];
        int ba = permutation[permutation[xi + 1] + yi];
        int bb = permutation[permutation[xi + 1] + yi + 1];

        double x1, x2, y1, x3, x4, y2;

        x1 = lerp(grad(aa, xf, yf), grad(ba, xf - 1, yf), u);
        x2 = lerp(grad(ab, xf, yf - 1), grad(bb, xf - 1, yf - 1), u);
        y1 = lerp(x1, x2, v);

        x3 = lerp(grad(aa, xf, yf), grad(ba, xf - 1, yf), u);
        x4 = lerp(grad(ab, xf, yf - 1), grad(bb, xf - 1, yf - 1), u);
        y2 = lerp(x3, x4, v);

        return (y1 + y2) / 2.0 + 0.5; // Нормализуем к [0, 1]
    }

    // Октавы (для детализации)
    double octave2D(double x, double y, int octaves, double persistence = 0.5) const {
        double total = 0;
        double frequency = 1;
        double amplitude = 1;
        double maxValue = 0;

        for (int i = 0; i < octaves; i++) {
            total += noise2D(x * frequency, y * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= 2;
        }

        return total / maxValue;
    }

private:
    uint8_t permutation[512];

    static double fade(double t) {
        return t * t * t * (t * (t * 6 - 15) + 10);
    }

    static double lerp(double a, double b, double t) {
        return a + t * (b - a);
    }

    static double grad(int hash, double x, double y)  {
        int h = hash & 3;
        double u = h < 2 ? x : y;
        double v = h < 2 ? y : x;
        return ((h & 1) ? -u : u) + ((h & 2) ? -2.0 * v : 2.0 * v);
    }
};

#endif //OPTICRAFT_PERLINNOISE_H
