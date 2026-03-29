//
// Created by noktemor on 11.02.2026.
//

#include "Game.h"
#include "utils/Logger.h"
#include "utils/Config.h"
#include <iostream>
#include <cmath>

Game::Game(int width, int height, const std::string& title)
    : m_is_running(false)
    , m_delta_time(0.0f)
    , m_last_frame_time(0.0f)
{
    LOG_INFO("Creating Game instance...");
}

Game::~Game() {
    shutdown();
}

bool Game::initialize() {
    LOG_INFO("Initializing Game...");

    // 1. Окно
    m_window_ptr = std::make_unique<Window>(
        Config::default_window_width,
        Config::default_window_height,
        Config::default_window_title
    );

    if (!m_window_ptr->initialize()) {
        LOG_ERROR("Failed to initialize window");
        return false;
    }

    // 2. Ввод
    m_input_ptr = std::make_unique<Input_Manager>(m_window_ptr->get_glfw_ptr());
    m_window_ptr->set_cursor_mode(GLFW_CURSOR_DISABLED);

    // Коллбэк мыши
    m_window_ptr->set_mouse_callback([this](double x, double y) {
        m_input_ptr->update_mouse(x, y);
    });

    // 3. Камера
    m_camera_ptr = std::make_unique<Camera>(
     glm::vec3(8.0f, 20.0f, 8.0f),  // позиция
     glm::vec3(0.0f, 1.0f, 0.0f),   // up
     -90.0f,                         // yaw (смотрит на +Z)
     -30.0f                          // pitch (смотрит вниз!)
 );

    // 4. Рендерер
    m_renderer_ptr = std::make_unique<Renderer>();

    std::string atlas_path =
    "/home/noktemor/CLionProjects/OptiCraft/assets/texture/texture_atlas.png";

    if (!m_renderer_ptr->initialize(
        "/home/noktemor/CLionProjects/OptiCraft/assets/shaders/vertex.glsl",
        "/home/noktemor/CLionProjects/OptiCraft/assets/shaders/fragment.glsl",
        atlas_path)) {
        LOG_ERROR("Failed to initialize renderer");
        return false;
        }

    // 5. Менеджер чанков
    m_chunk_manager_ptr = std::make_unique<Chunk_Manager>();

    // 6. Игрок
    m_player_ptr = std::make_unique<Player>();
    m_player_ptr->set_position(glm::vec3(8.0f, 20.0f, 8.0f));

    // Начальная загрузка чанков
    auto player_pos = m_player_ptr->get_position();
    int player_cx = static_cast<int>(std::floor(player_pos.x / Config::chunk_size));
    int player_cz = static_cast<int>(std::floor(player_pos.z / Config::chunk_size));
    m_chunk_manager_ptr->load_around(player_cx, player_cz);

    m_is_running = true;
    LOG_INFO("Game initialized successfully");
    return true;
}

void Game::run() {
    LOG_INFO("Starting game loop...");

    while (m_is_running && !m_window_ptr->should_close()) {
        // 1. Время
        float current_time = static_cast<float>(glfwGetTime());
        m_delta_time = current_time - m_last_frame_time;
        m_last_frame_time = current_time;
        m_delta_time = std::clamp(m_delta_time, 0.001f, 0.1f);

        // 2. Ввод
        handle_input();

        // 3. Обновление мира
        update_world();

        // 4. Рендер
        render_frame();

        // 5. Завершение кадра
        m_window_ptr->swap_buffers();
        m_window_ptr->poll_events();
    }

    shutdown();
}

void Game::handle_input() {
    m_input_ptr->update();

    // ESC
    if (m_input_ptr->is_key_pressed(GLFW_KEY_ESCAPE)) {
        m_is_running = false;
        return;
    }

    // Поворот камеры
    auto mouse_delta = m_input_ptr->get_mouse_delta();
    m_camera_ptr->rotate(mouse_delta.x, mouse_delta.y);
    m_input_ptr->reset_mouse_delta();

    // Движение игрока
    auto move_input = m_input_ptr->get_movement_input();
    m_player_ptr->set_movement_input(move_input);
}

void Game::update_world() {
    // Обновление игрока
    m_player_ptr->update(m_delta_time, *m_chunk_manager_ptr);

    // Камера следует за игроком
    m_camera_ptr->set_position(
        m_player_ptr->get_position() + glm::vec3(0.0f, m_player_ptr->get_eye_height(), 0.0f)
    );

    // Обновление чанков
    auto player_pos = m_player_ptr->get_position();
    int player_cx = static_cast<int>(std::floor(player_pos.x / Config::chunk_size));
    int player_cz = static_cast<int>(std::floor(player_pos.z / Config::chunk_size));
    m_chunk_manager_ptr->update_player_position(player_pos.x, player_pos.z);

    // Пересборка изменённых чанков (если нужно)
    // ...
}

void Game::render_frame() {
    m_renderer_ptr->clear_screen(
        Config::clear_color_r,
        Config::clear_color_g,
        Config::clear_color_b
    );

    float aspect_ratio = static_cast<float>(m_window_ptr->get_width()) /
                        static_cast<float>(m_window_ptr->get_height());

    auto view = m_camera_ptr->get_view_matrix();
    auto projection = m_camera_ptr->get_projection_matrix(aspect_ratio);

    auto renderable = m_chunk_manager_ptr->get_renderable_chunks();

    size_t total_vertices = 0;
    for (const auto& [pos, vertices] : renderable) {
        total_vertices += vertices.size();
    }
    std::cout << "[DEBUG] Rendering " << renderable.size()
              << " chunks, " << total_vertices << " vertices\n";

    m_renderer_ptr->render_chunks(renderable, view, projection);

    // Статистика (раз в 60 кадров)
    static int frame_counter = 0;
    if (++frame_counter % 60 == 0) {
        log_stats();
    }
    static int debug_counter = 0;
    if (++debug_counter % 60 == 0) {
        auto pos = m_camera_ptr->get_position();
        auto front = m_camera_ptr->get_front();
        std::cout << "[CAMERA] Pos: (" << pos.x << ", " << pos.y << ", " << pos.z << ")\n";
        std::cout << "[CAMERA] Front: (" << front.x << ", " << front.y << ", " << front.z << ")\n";

        // Куда смотрит камера? (точка на расстоянии 10 единиц)
        auto look_at = pos + front * 10.0f;
        std::cout << "[CAMERA] Looking at: (" << look_at.x << ", " << look_at.y << ", " << look_at.z << ")\n";
        std::cout << "-------------\n";
    }
}

void Game::log_stats() {
    std::cout << "\n=== Stats ===\n";
    std::cout << "Chunks loaded: " << m_chunk_manager_ptr->get_loaded_count() << "\n";
    std::cout << "Player pos: "
              << static_cast<int>(m_player_ptr->get_position().x) << ", "
              << static_cast<int>(m_player_ptr->get_position().y) << ", "
              << static_cast<int>(m_player_ptr->get_position().z) << "\n";
    std::cout << "=============\n\n";
}

void Game::shutdown() {
    LOG_INFO("Shutting down...");
    m_is_running = false;

    // 1. Сначала очищаем OpenGL-ресурсы (пока контекст жив!)
    m_renderer_ptr.reset();      // Удалит шейдеры, VAO, VBO
    m_chunk_manager_ptr.reset(); // Удалит чанки
    m_player_ptr.reset();
    m_camera_ptr.reset();
    m_input_ptr.reset();

    // 2. Только потом уничтожаем окно и GLFW
    m_window_ptr.reset();        // Уничтожит окно (glfwDestroyWindow)
    glfwTerminate();             // Завершит GLFW

    LOG_INFO("OptiCraft exited");
}