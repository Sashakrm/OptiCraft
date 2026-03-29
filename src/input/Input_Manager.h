//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_INPUT_MANAGER_H
#define OPTICRAFT_INPUT_MANAGER_H

#include <GLFW/glfw3.h>
#include <unordered_map>

class Input_Manager {
private:
    GLFWwindow* m_window_ptr;

    // Состояния клавиш
    std::unordered_map<int, bool> m_key_current;
    std::unordered_map<int, bool> m_key_previous;

    // Мышь
    bool m_is_first_mouse;
    float m_last_mouse_x;
    float m_last_mouse_y;
    float m_mouse_delta_x;
    float m_mouse_delta_y;

    // Настройки
    float m_mouse_sensitivity;

    // Маппинг клавиш (можно грузить из конфига)
    int m_key_forward;
    int m_key_backward;
    int m_key_left;
    int m_key_right;
    int m_key_jump;

public:
    Input_Manager(GLFWwindow* window_ptr);

    void update(); // Вызывать каждый кадр

    // Проверка состояния клавиш
    bool is_key_pressed(int key) const;      // Только в этом кадре нажата
    bool is_key_held(int key) const;         // Удерживается
    bool is_key_released(int key) const;     // Только в этом кадре отпущена

    // Ввод движения
    struct Move_Input {
        float forward;  // W/S: -1..1
        float strafe;   // A/D: -1..1
        bool jump;
    };
    Move_Input get_movement_input() const;

    // Ввод мыши
    struct Mouse_Delta {
        float x;
        float y;
    };
    void update_mouse(double xpos, double ypos);
    Mouse_Delta get_mouse_delta() const;
    void reset_mouse_delta();

    // Настройки
    void set_mouse_sensitivity(float sens) { m_mouse_sensitivity = sens; }
    float get_mouse_sensitivity() const { return m_mouse_sensitivity; }

    void set_key_binding(int action, int key);
    int get_key_binding(int action) const;
};

#endif //OPTICRAFT_INPUT_MANAGER_H
