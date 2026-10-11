//
// Created by noktemor on 28.03.2026.
//

#include "entities/player/Camera.h"
#include "utils/Config.h"
#include "math/Math_Helpers.h"

#include <cmath>

Camera::Camera(const glm::vec3& position, const glm::vec3& up, float yaw, float pitch)
    : m_position(position)
    , m_world_up(up)
    , m_yaw(yaw)
    , m_pitch(pitch)
    , m_fov(Config::camera_fov)
    , m_near(Config::camera_near)
    , m_far(Config::camera_far)
    , m_move_speed(Config::player_move_speed)
{
    update_camera_vectors();
}

void Camera::update_camera_vectors() {
    glm::vec3 front;
    front.x = cos(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    front.y = sin(glm::radians(m_pitch));
    front.z = sin(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
    m_front = glm::normalize(front);

    m_right = glm::normalize(glm::cross(m_front, m_world_up));
    m_up = glm::normalize(glm::cross(m_right, m_front));
}

glm::mat4 Camera::get_view_matrix() const {
    return glm::lookAt(m_position, m_position + m_front, m_up);
}

glm::mat4 Camera::get_projection_matrix(float aspect_ratio) const {
    return glm::perspective(glm::radians(m_fov), aspect_ratio, m_near, m_far);
}

void Camera::set_position(const glm::vec3& position) {
    m_position = position;
}

void Camera::look_at(const glm::vec3& target) {
    const glm::vec3 offset = target - m_position;
    if (glm::length(offset) < 0.0001f) {
        return;
    }
    const glm::vec3 direction = glm::normalize(offset);
    m_pitch = glm::degrees(std::asin(direction.y));
    m_yaw = glm::degrees(std::atan2(direction.z, direction.x));
    update_camera_vectors();
}

void Camera::rotate(float x_offset, float y_offset, bool constrain_pitch) {
    m_yaw += x_offset;
    m_pitch += y_offset;

    if (constrain_pitch) {
        m_pitch = Math_Helpers::clamp(m_pitch, -89.0f, 89.0f);
    }

    update_camera_vectors();
}

void Camera::process_keyboard(int direction, float delta_time) {
    float velocity = m_move_speed * delta_time;

    if (direction == 0) // FORWARD
        m_position += m_front * velocity;
    else if (direction == 1) // BACKWARD
        m_position -= m_front * velocity;
    else if (direction == 2) // LEFT
        m_position -= m_right * velocity;
    else if (direction == 3) // RIGHT
        m_position += m_right * velocity;
}
