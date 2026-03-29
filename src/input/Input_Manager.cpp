//
// Created by noktemor on 28.03.2026.
//

#include "Input_Manager.h"
#include "utils/Config.h"

Input_Manager::Input_Manager(GLFWwindow* window_ptr)
    : m_window_ptr(window_ptr)
    , m_is_first_mouse(true)
    , m_last_mouse_x(0.0f)
    , m_last_mouse_y(0.0f)
    , m_mouse_delta_x(0.0f)
    , m_mouse_delta_y(0.0f)
    , m_mouse_sensitivity(Config::mouse_sensitivity)
    , m_key_forward(GLFW_KEY_W)
    , m_key_backward(GLFW_KEY_S)
    , m_key_left(GLFW_KEY_A)
    , m_key_right(GLFW_KEY_D)
    , m_key_jump(GLFW_KEY_SPACE)
{}

void Input_Manager::update() {
    // Сохраняем предыдущее состояние клавиш
    m_key_previous = m_key_current;

    // Обновляем текущее состояние
    for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; ++key) {
        m_key_current[key] = (glfwGetKey(m_window_ptr, key) == GLFW_PRESS);
    }
}

bool Input_Manager::is_key_pressed(int key) const {
    auto it_curr = m_key_current.find(key);
    auto it_prev = m_key_previous.find(key);
    return (it_curr != m_key_current.end() && it_curr->second) &&
           (it_prev == m_key_previous.end() || !it_prev->second);
}

bool Input_Manager::is_key_held(int key) const {
    auto it = m_key_current.find(key);
    return (it != m_key_current.end() && it->second);
}

bool Input_Manager::is_key_released(int key) const {
    auto it_curr = m_key_current.find(key);
    auto it_prev = m_key_previous.find(key);
    return (it_prev != m_key_previous.end() && it_prev->second) &&
           (it_curr == m_key_current.end() || !it_curr->second);
}

Input_Manager::Move_Input Input_Manager::get_movement_input() const {
    Move_Input input{0.0f, 0.0f, false};

    if (is_key_held(m_key_forward)) input.forward += 1.0f;
    if (is_key_held(m_key_backward)) input.forward -= 1.0f;
    if (is_key_held(m_key_left)) input.strafe -= 1.0f;
    if (is_key_held(m_key_right)) input.strafe += 1.0f;
    if (is_key_pressed(m_key_jump)) input.jump = true;

    return input;
}

void Input_Manager::update_mouse(double xpos, double ypos) {
    if (m_is_first_mouse) {
        m_last_mouse_x = static_cast<float>(xpos);
        m_last_mouse_y = static_cast<float>(ypos);
        m_is_first_mouse = false;
        m_mouse_delta_x = 0.0f;
        m_mouse_delta_y = 0.0f;
        return;
    }

    m_mouse_delta_x = (static_cast<float>(xpos) - m_last_mouse_x) * m_mouse_sensitivity;
    m_mouse_delta_y = (m_last_mouse_y - static_cast<float>(ypos)) * m_mouse_sensitivity;

    m_last_mouse_x = static_cast<float>(xpos);
    m_last_mouse_y = static_cast<float>(ypos);
}

Input_Manager::Mouse_Delta Input_Manager::get_mouse_delta() const {
    return {m_mouse_delta_x, m_mouse_delta_y};
}

void Input_Manager::reset_mouse_delta() {
    m_mouse_delta_x = 0.0f;
    m_mouse_delta_y = 0.0f;
}

void Input_Manager::set_key_binding(int action, int key) {
    switch (action) {
        case 0: m_key_forward = key; break;
        case 1: m_key_backward = key; break;
        case 2: m_key_left = key; break;
        case 3: m_key_right = key; break;
        case 4: m_key_jump = key; break;
    }
}

int Input_Manager::get_key_binding(int action) const {
    switch (action) {
        case 0: return m_key_forward;
        case 1: return m_key_backward;
        case 2: return m_key_left;
        case 3: return m_key_right;
        case 4: return m_key_jump;
        default: return -1;
    }
}