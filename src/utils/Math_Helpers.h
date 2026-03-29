//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_MATH_HELPER_H
#define OPTICRAFT_MATH_HELPER_H

#include <glm/glm.hpp>
#include <cmath>

class Math_Helpers {
public:
    static inline float clamp(float value, float min_val, float max_val) {
        return (value < min_val) ? min_val : (value > max_val) ? max_val : value;
    }

    static inline glm::vec3 clamp_vec3(const glm::vec3& v, float min_val, float max_val) {
        return glm::vec3(
            clamp(v.x, min_val, max_val),
            clamp(v.y, min_val, max_val),
            clamp(v.z, min_val, max_val)
        );
    }

    static inline bool approximately_equal(float a, float b, float epsilon = 0.001f) {
        return std::abs(a - b) < epsilon;
    }

    static inline int floor_div(int a, int b) {
        return (a >= 0) ? (a / b) : ((a - b + 1) / b);
    }
};

#endif //OPTICRAFT_MATH_HELPER_H
