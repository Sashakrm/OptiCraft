//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_CONFIG_H
#define OPTICRAFT_CONFIG_H

namespace Config {

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

    // 🌍 Вертикальные границы
    constexpr int world_min_y = 5;
    constexpr int world_sea_level = 63;
    constexpr int world_max_y = 512;
    constexpr int world_vertical_range = world_max_y - world_min_y;

    // 🏔️ Параметры генерации
    constexpr float terrain_base_scale = 0.008f;
    constexpr float terrain_detail_scale = 0.03f;
    constexpr float terrain_height_multiplier = 150.0f;

    // 🌊 Океан
    constexpr float ocean_depth = 15.0f;

    // 👁️ Дистанция прорисовки
    constexpr int chunk_load_radius = 3;

    // 🎲 Генерация
    constexpr int world_seed = 12345;
    constexpr float tree_spawn_chance = 0.02f;  // 2% шанс на дерево

    // 🧱 Чанк
    constexpr int chunk_size = 16;
    constexpr int chunk_height = 512;  // Должен покрыть world_vertical_range (768)
    // Примечание: возможно понадобится 512 или динамическое
}

#endif //OPTICRAFT_CONFIG_H
