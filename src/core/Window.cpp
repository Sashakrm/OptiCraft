//
// Created by noktemor on 28.03.2026.
//

#include <glad/gl.h>
#include "Window.h"
#include "utils/Logger.h"


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

    if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
        LOG_ERROR("Failed to initialize GLAD");
        return false;
    }

    // Настройки OpenGL
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDepthFunc(GL_LESS);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);

    // Регистрация статических коллбэков
    glfwSetCursorPosCallback(m_window_ptr, mouse_callback_static);
    glfwSetKeyCallback(m_window_ptr, key_callback_static);
    glfwSetWindowUserPointer(m_window_ptr, this);

    m_is_open = true;
    LOG_INFO("Window initialized: " + m_title);
    return true;
}

bool Window::should_close() const {
    return glfwWindowShouldClose(m_window_ptr);
}

void Window::swap_buffers() {
    glfwSwapBuffers(m_window_ptr);
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

// Статические коллбэки
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
