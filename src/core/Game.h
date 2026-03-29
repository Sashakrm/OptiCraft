//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_GAME_H
#define OPTICRAFT_GAME_H

#define GLFW_INCLUDE_NONE

#include "Window.h"
#include "input/Input_Manager.h"
#include "entities/Camera.h"
#include "rendering/Renderer.h"
#include "world/Chunk_Manager.h"
#include "entities/Player.h"
#include <memory>

class Game {
private:
    // Модули
    std::unique_ptr<Window> m_window_ptr;
    std::unique_ptr<Input_Manager> m_input_ptr;
    std::unique_ptr<Camera> m_camera_ptr;
    std::unique_ptr<Renderer> m_renderer_ptr;
    std::unique_ptr<Chunk_Manager> m_chunk_manager_ptr;
    std::unique_ptr<Player> m_player_ptr;

    // Состояние цикла
    bool m_is_running;
    float m_delta_time;
    float m_last_frame_time;

    // Внутренние методы
    void init_modules();
    void handle_input();
    void update_world();
    void render_frame();
    void log_stats();

public:
    Game(int width, int height, const std::string& title);
    ~Game();

    bool initialize();
    void run();
    void shutdown();
};

#endif //OPTICRAFT_GAME_H