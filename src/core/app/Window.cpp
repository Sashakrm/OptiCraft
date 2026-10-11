//
// Created by noktemor on 28.03.2026.
//

#include <glad/gl.h>
#include "core/app/Window.h"
#include "utils/Config.h"
#include "utils/logging/Logger.h"


Window::Window(int width, int height, const std::string& title)
    : m_window_ptr(nullptr)
    , m_width(width)
    , m_height(height)
    , m_title(title)
    , m_is_open(false)
{}

Window::~Window() {
    if (m_window_ptr) {
        glfwDestroyWindow(m_window_ptr);
    }
    // glfwTerminate() вызывается в Game::~Game
}

bool Window::initialize() {
    if (!glfwInit()) {
        LOG_ERROR("Failed to initialize GLFW");
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    m_window_ptr = glfwCreateWindow(m_width, m_height, m_title.c_str(), nullptr, nullptr);
    if (!m_window_ptr) {
        LOG_ERROR("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(m_window_ptr);
    // Make VSync explicit: if swap time remains near 16.6 ms with this disabled,
    // the remaining 60 FPS cap is imposed by the compositor or graphics driver.
    glfwSwapInterval(Config::vsync_enabled ? 1 : 0);
    LOG_INFO(std::string("VSync: ") + (Config::vsync_enabled ? "on" : "off"));

    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
        LOG_ERROR("Failed to initialize GLAD");
        return false;
    }

    // Настройки OpenGL
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDepthFunc(GL_LESS);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // Регистрация статических коллбэков
    glfwSetWindowUserPointer(m_window_ptr, this);
    glfwSetCursorPosCallback(m_window_ptr, mouse_callback_static);
    glfwSetKeyCallback(m_window_ptr, key_callback_static);
    glfwSetCharCallback(m_window_ptr, char_callback_static);
    glfwSetScrollCallback(m_window_ptr, scroll_callback_static);
    glfwSetFramebufferSizeCallback(m_window_ptr, framebuffer_size_callback_static);
    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(m_window_ptr, &framebuffer_width, &framebuffer_height);
    update_framebuffer_size(framebuffer_width, framebuffer_height);

    m_is_open = true;
    LOG_INFO("Window initialized: " + m_title);
    return true;
}

bool Window::should_close() const {
    return glfwWindowShouldClose(m_window_ptr);
}

void Window::swap_buffers() {
    const double start_time = glfwGetTime();
    glfwSwapBuffers(m_window_ptr);
    const double elapsed = glfwGetTime() - start_time;
    m_swap_time_sum += elapsed;
    ++m_swap_time_samples;
    if (m_swap_time_samples == 120) {
        const double average_ms = (m_swap_time_sum / m_swap_time_samples) * 1000.0;
        LOG_INFO("Average glfwSwapBuffers time: " + std::to_string(average_ms) + " ms");
        m_swap_time_sum = 0.0;
        m_swap_time_samples = 0;
    }
}

void Window::poll_events() {
    glfwPollEvents();
}

void Window::set_cursor_mode(int mode) {
    glfwSetInputMode(m_window_ptr, GLFW_CURSOR, mode);
}

void Window::set_window_user_pointer(void* ptr) {
    glfwSetWindowUserPointer(m_window_ptr, ptr);
}

void Window::set_mouse_callback(std::function<void(double, double)> callback) {
    m_mouse_callback = callback;
}

void Window::set_key_callback(std::function<void(int, int, int)> callback) {
    m_key_callback = callback;
}

void Window::set_char_callback(std::function<void(unsigned int)> callback) {
    m_char_callback = callback;
}

void Window::set_scroll_callback(std::function<void(double, double)> callback) {
    m_scroll_callback = callback;
}

void Window::set_title(const std::string& title) {
    m_title = title;
    if (m_window_ptr) glfwSetWindowTitle(m_window_ptr, m_title.c_str());
}

// Статические коллбэки
void Window::scroll_callback_static(GLFWwindow* window, double xoffset, double yoffset) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->m_scroll_callback) {
        self->m_scroll_callback(xoffset, yoffset);
    }
}
void Window::mouse_callback_static(GLFWwindow* window, double xpos, double ypos) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->m_mouse_callback) {
        self->m_mouse_callback(xpos, ypos);
    }
}

void Window::key_callback_static(GLFWwindow* window, int key, int scancode, int action, int mods) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->m_key_callback) {
        self->m_key_callback(key, action, mods);
    }
}

void Window::char_callback_static(GLFWwindow* window, unsigned int codepoint) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self && self->m_char_callback) {
        self->m_char_callback(codepoint);
    }
}

void Window::framebuffer_size_callback_static(GLFWwindow* window, int width, int height) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self) {
        self->update_framebuffer_size(width, height);
    }
}

void Window::update_framebuffer_size(int width, int height) {
    m_width = width;
    m_height = height;
    glViewport(0, 0, width, height);
}
