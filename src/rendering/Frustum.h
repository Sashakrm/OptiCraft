#ifndef OPTICRAFT_FRUSTUM_H
#define OPTICRAFT_FRUSTUM_H

#include <array>
#include <glm/glm.hpp>

class Frustum {
private:
    std::array<glm::vec4, 6> m_planes{};

    void set_plane(size_t index, const glm::vec4& plane) {
        const float length = glm::length(glm::vec3(plane));
        m_planes[index] = length > 0.0f ? plane / length : plane;
    }

public:
    explicit Frustum(const glm::mat4& clip) {
        // GLM stores matrices column-major: clip[column][row].
        const glm::vec4 row0(clip[0][0], clip[1][0], clip[2][0], clip[3][0]);
        const glm::vec4 row1(clip[0][1], clip[1][1], clip[2][1], clip[3][1]);
        const glm::vec4 row2(clip[0][2], clip[1][2], clip[2][2], clip[3][2]);
        const glm::vec4 row3(clip[0][3], clip[1][3], clip[2][3], clip[3][3]);
        set_plane(0, row3 + row0); // left
        set_plane(1, row3 - row0); // right
        set_plane(2, row3 + row1); // bottom
        set_plane(3, row3 - row1); // top
        set_plane(4, row3 + row2); // near
        set_plane(5, row3 - row2); // far
    }

    const std::array<glm::vec4, 6>& planes() const { return m_planes; }

    bool intersects_aabb(const glm::vec3& min, const glm::vec3& max) const {
        for (const glm::vec4& plane : m_planes) {
            const glm::vec3 positive(
                plane.x >= 0.0f ? max.x : min.x,
                plane.y >= 0.0f ? max.y : min.y,
                plane.z >= 0.0f ? max.z : min.z
            );
            if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f) return false;
        }
        return true;
    }
};

#endif // OPTICRAFT_FRUSTUM_H
