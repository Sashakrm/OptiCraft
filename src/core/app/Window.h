//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_WINDOW_H
#define OPTICRAFT_WINDOW_H

#include <GLFW/glfw3.h>
#include <string>
#include <functional>

class Window {
private:
    GLFWwindow* m_window_ptr;
    int m_width;
    int m_height;
    std::string m_title;
    bool m_is_open;
    double m_swap_time_sum = 0.0;
    int m_swap_time_samples = 0;

    // Коллбэки
    std::function<void(double, double)> m_mouse_callback;
    std::function<void(int, int, int)> m_key_callback;
    std::function<void(unsigned int)> m_char_callback;
    std::function<void(double, double)> m_scroll_callback;

    // Статические коллбэки для GLFW
    static void mouse_callback_static(GLFWwindow* window, double xpos, double ypos);
    static void key_callback_static(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void char_callback_static(GLFWwindow* window, unsigned int codepoint);
    static void scroll_callback_static(GLFWwindow* window, double xoffset, double yoffset);
    static void framebuffer_size_callback_static(GLFWwindow* window, int width, int height);
    void update_framebuffer_size(int width, int height);

public:
    Window(int width, int height, const std::string& title);
    ~Window();

    // Запрет копирования
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool initialize();
    bool should_close() const;
    void swap_buffers();
    void poll_events();

    // Геттеры
    GLFWwindow* get_glfw_ptr() { return m_window_ptr; }
    int get_width() const { return m_width; }
    int get_height() const { return m_height; }
    bool is_open() const { return m_is_open; }

    // Настройки
    void set_cursor_mode(int mode);
    void set_window_user_pointer(void* ptr);

    // Регистрация коллбэков
    void set_mouse_callback(std::function<void(double, double)> callback);
    void set_key_callback(std::function<void(int, int, int)> callback);
    // Ввод текста (символ уже с учётом раскладки/Shift) — нужен чату.
    void set_char_callback(std::function<void(unsigned int)> callback);
    // Колесо мыши: прокрутка списка миров и смена слота хотбара.
    void set_scroll_callback(std::function<void(double, double)> callback);
    void set_title(const std::string& title);
};

#endif //OPTICRAFT_WINDOW_H
