//
// Texture_Atlas — одна текстура + именованные спрайты внутри неё, описанные
// явным пиксельным прямоугольником (x, y, w, h), а НЕ равномерной сеткой.
//
// Раньше атлас был жёстко "сеткой tiles_per_row x tiles_per_column" с
// одинаковым размером каждого тайла — из-за этого текстуру нельзя было
// сделать не-16x16 (факел, например, никогда не квадратный 1:1 по смыслу
// рисунка). Теперь каждый спрайт — произвольный прямоугольник в пикселях
// самого PNG, так что в одном атласе спокойно уживаются тайлы 16x16,
// 16x24, 32x32 и т.д. — кто как нарисован.
//
// Атлас грузится из манифеста (.atlas.json) — см. Atlas_Registry, которая
// сканирует assets/atlases/ и грузит ЛЮБОЕ количество атласов без единой
// правки кода: чтобы добавить новый атлас, достаточно положить туда PNG +
// .atlas.json, пересборка не нужна.
//
#ifndef OPTICRAFT_TEXTURE_ATLAS_H
#define OPTICRAFT_TEXTURE_ATLAS_H

#include "rendering/atlas/Texture.h"
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>

class Texture_Atlas {
private:
    Texture m_texture;
    std::string m_name; // из поля "name" манифеста — как атлас зарегистрирован в Atlas_Registry

    struct Sprite_Rect {
        float x, y, w, h; // в пикселях исходного PNG, (0,0) — верхний левый угол
    };
    std::unordered_map<std::string, Sprite_Rect> m_sprites;

public:
    Texture_Atlas() = default;

    // Грузит атлас из .atlas.json (см. формат в Atlas_Registry.h). Путь к текстуре
    // внутри манифеста — относительно самого манифеста.
    bool load_from_manifest(const std::string& manifest_path);

    // UV-прямоугольник (u_min, v_min, u_max, v_max) для спрайта по имени. Если такого
    // спрайта нет — пишет в лог и возвращает (0,0,0,0) (виден как "дырка", не падение).
    glm::vec4 get_uv_coords(const std::string& sprite_name) const;
    // Пиксельный прямоугольник спрайта (x, y, w, h) и размер текстуры — нужны 9-slice панелям UI.
    bool get_sprite_pixel_rect(const std::string& sprite_name, glm::vec4& out_rect) const;
    glm::vec2 get_texture_size() const { return glm::vec2(static_cast<float>(m_texture.get_width()), static_cast<float>(m_texture.get_height())); }
    bool has_sprite(const std::string& sprite_name) const { return m_sprites.count(sprite_name) != 0; }

    void bind(unsigned int texture_unit = 0) const { m_texture.bind(texture_unit); }
    void unbind() const { m_texture.unbind(); }

    bool is_loaded() const { return m_texture.is_loaded(); }
    const std::string& get_name() const { return m_name; }
    const Texture& get_texture() const { return m_texture; }
};

#endif //OPTICRAFT_TEXTURE_ATLAS_H
