//
// Created by noktemor on 28.03.2026.
//

#include "Texture_Atlas.h"
#include "utils/Logger.h"

Texture_Atlas::Texture_Atlas()
    : m_tiles_per_row(0)
    , m_tiles_per_column(0)
    , m_tile_width_uv(0.0f)
    , m_tile_height_uv(0.0f)
{}

bool Texture_Atlas::load_from_file(const std::string& path, int tiles_per_row, int tiles_per_column) {
    if (!m_texture.load_from_file(path)) {
        return false;
    }

    m_tiles_per_row = tiles_per_row;
    m_tiles_per_column = tiles_per_column;
    m_tile_width_uv = 1.0f / static_cast<float>(tiles_per_row);
    m_tile_height_uv = 1.0f / static_cast<float>(tiles_per_column);

    LOG_INFO("Texture atlas loaded: " + path +
             " (" + std::to_string(tiles_per_row) + "x" + std::to_string(tiles_per_column) + " tiles)");
    return true;
}

glm::vec4 Texture_Atlas::get_uv_coords(int tile_index) const {
    // Индекс → координаты в сетке атласа
    int row = tile_index / m_tiles_per_row;
    int col = tile_index % m_tiles_per_row;

    float u_min = static_cast<float>(col) * m_tile_width_uv;
    float v_min = 1.0f - static_cast<float>(row + 1) * m_tile_height_uv;  // OpenGL V идёт снизу-вверх
    float u_max = u_min + m_tile_width_uv;
    float v_max = v_min + m_tile_height_uv;

    return glm::vec4(u_min, v_min, u_max, v_max);
}
