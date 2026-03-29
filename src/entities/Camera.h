//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_CAMERA_H
#define OPTICRAFT_CAMERA_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Camera {
private:
    glm::vec3 m_position;
    glm::vec3 m_front;
    glm::vec3 m_up;
    glm::vec3 m_right;
    glm::vec3 m_world_up;

    // Углы
    float m_yaw;
    float m_pitch;

    // Параметры
    float m_fov;
    float m_near;
    float m_far;
    float m_move_speed;
    float m_mouse_sensitivity;

public:
    Camera(const glm::vec3& position = glm::vec3(0.0f, 0.0f, 0.0f),
           const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f),
           float yaw = -90.0f,
           float pitch = 0.0f);
    void update_camera_vectors();

    // Матрицы
    glm::mat4 get_view_matrix() const;
    glm::mat4 get_projection_matrix(float aspect_ratio) const;

    // Управление
    void set_position(const glm::vec3& position);
    void look_at(const glm::vec3& target);
    void rotate(float x_offset, float y_offset, bool constrain_pitch = true);
    void process_keyboard(int direction, float delta_time);

    // Геттеры
    const glm::vec3& get_position() const { return m_position; }
    const glm::vec3& get_front() const { return m_front; }
    const glm::vec3& get_up() const { return m_up; }
    const glm::vec3& get_right() const { return m_right; }
    float get_yaw() const { return m_yaw; }
    float get_pitch() const { return m_pitch; }

    // Настройки
    void set_fov(float fov) { m_fov = fov; }
    void set_sensitivity(float sens) { m_mouse_sensitivity = sens; }
    void set_move_speed(float speed) { m_move_speed = speed; }
};

#endif //OPTICRAFT_CAMERA_H
