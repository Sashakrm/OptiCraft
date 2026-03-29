//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_TEXTURE_ATLAS_H
#define OPTICRAFT_TEXTURE_ATLAS_H

#include "utils/Texture.h"
#include <glm/glm.hpp>

class Texture_Atlas {
private:
    Texture m_texture;

    int m_tiles_per_row;
    int m_tiles_per_column;
    float m_tile_width_uv;   // 1.0 / tiles_per_row
    float m_tile_height_uv;  // 1.0 / tiles_per_column

public:
    Texture_Atlas();

    bool load_from_file(const std::string& path, int tiles_per_row, int tiles_per_column);

    // Получить UV-координаты для тайла по индексу
    // Индекс: 0 = верхний-левый, идёт слева-направо, сверху-вниз
    glm::vec4 get_uv_coords(int tile_index) const;

    // Привязка текстуры
    void bind(unsigned int texture_unit = 0) const { m_texture.bind(texture_unit); }
    void unbind() const { m_texture.unbind(); }

    bool is_loaded() const { return m_texture.is_loaded(); }
    int get_tiles_per_row() const { return m_tiles_per_row; }
    int get_tiles_per_column() const { return m_tiles_per_column; }
};

#endif //OPTICRAFT_TEXTURE_ATLAS_H
