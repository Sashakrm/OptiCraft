//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_CONFIG_H
#define OPTICRAFT_CONFIG_H

namespace Config {
    // Мир
    constexpr int chunk_size = 16;
    constexpr int chunk_height = 16;
    constexpr int chunk_load_radius = 1;

    // Игрок
    constexpr float player_width = 0.6f;
    constexpr float player_height = 1.8f;
    constexpr float player_eye_height = 1.62f;
    constexpr float player_move_speed = 5.0f;
    constexpr float player_jump_force = 7.5f;
    constexpr float player_gravity = -20.0f;

    // Камера
    constexpr float camera_fov = 70.0f;
    constexpr float camera_near = 0.1f;
    constexpr float camera_far = 1000.0f;
    constexpr float mouse_sensitivity = 0.35f;

    // Рендер
    constexpr float clear_color_r = 0.45f;
    constexpr float clear_color_g = 0.68f;
    constexpr float clear_color_b = 1.0f;

    // Окно
    constexpr int default_window_width = 1280;
    constexpr int default_window_height = 720;
    constexpr const char* default_window_title = "OptiCraft";
}

#endif //OPTICRAFT_CONFIG_H
