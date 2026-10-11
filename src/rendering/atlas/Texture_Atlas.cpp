#include "rendering/atlas/Texture_Atlas.h"
#include "utils/logging/Logger.h"
#include "utils/text/Json.h"

#include <filesystem>

namespace {
std::string parent_directory(const std::string& path) {
    const std::filesystem::path p(path);
    return p.has_parent_path() ? p.parent_path().string() : ".";
}

// value-or-default для полей, у которых Json_Value::find может вернуть nullptr.
const Json_Value& field_or_null(const Json_Value* v) {
    static const Json_Value k_null{};
    return v ? *v : k_null;
}
} // namespace

bool Texture_Atlas::load_from_manifest(const std::string& manifest_path) {
    Json_Value root;
    try {
        root = parse_json_file(manifest_path);
    } catch (const std::exception& e) {
        LOG_ERROR("Texture_Atlas: failed to parse manifest " + manifest_path + " (" + e.what() + ")");
        return false;
    }
    if (!root.is_object()) {
        LOG_ERROR("Texture_Atlas: manifest is not a JSON object: " + manifest_path);
        return false;
    }

    m_name = json_read_string(field_or_null(root.find("name")), "");

    const Json_Value* texture_field = root.find("texture");
    if (!texture_field || !texture_field->is_string()) {
        LOG_ERROR("Texture_Atlas: manifest missing \"texture\": " + manifest_path);
        return false;
    }

    // Путь к PNG — относительно самого манифеста, чтобы можно было переносить
    // папку атласа целиком (PNG + .atlas.json) без правки путей внутри.
    const std::string texture_path = parent_directory(manifest_path) + "/" + texture_field->string_value;
    if (!m_texture.load_from_file(texture_path)) {
        LOG_ERROR("Texture_Atlas: failed to load texture " + texture_path + " for manifest " + manifest_path);
        return false;
    }

    const float tex_w = static_cast<float>(m_texture.get_width());
    const float tex_h = static_cast<float>(m_texture.get_height());
    (void)tex_w; (void)tex_h; // используются ниже только для лога размеров

    const Json_Value* sprites = root.find("sprites");
    if (sprites && sprites->is_object()) {
        for (const auto& [sprite_name, rect] : sprites->object_value) {
            if (!rect.is_object()) continue;
            Sprite_Rect r{};
            r.x = json_read_float(field_or_null(rect.find("x")), 0.0f);
            r.y = json_read_float(field_or_null(rect.find("y")), 0.0f);
            r.w = json_read_float(field_or_null(rect.find("w")), 0.0f);
            r.h = json_read_float(field_or_null(rect.find("h")), 0.0f);
            m_sprites[sprite_name] = r;
        }
    }

    LOG_INFO("Texture_Atlas loaded: " + manifest_path + " (\"" + m_name + "\", " +
             std::to_string(m_sprites.size()) + " sprites, " +
             std::to_string(m_texture.get_width()) + "x" + std::to_string(m_texture.get_height()) + " px)");
    return true;
}

glm::vec4 Texture_Atlas::get_uv_coords(const std::string& sprite_name) const {
    const auto it = m_sprites.find(sprite_name);
    if (it == m_sprites.end()) {
        LOG_ERROR("Texture_Atlas[\"" + m_name + "\"]: unknown sprite \"" + sprite_name + "\"");
        return glm::vec4(0.0f);
    }

    const Sprite_Rect& r = it->second;
    const float tex_w = static_cast<float>(m_texture.get_width());
    const float tex_h = static_cast<float>(m_texture.get_height());
    if (tex_w <= 0.0f || tex_h <= 0.0f) return glm::vec4(0.0f);

    // Texture::load_from_file грузит с вертикальным флипом (см. её комментарии), поэтому
    // v=1 соответствует ВЕРХУ картинки, v=0 — низу. Пиксельный y растёт вниз (y=0 — верх),
    // так что верх спрайта (y) — это v_max, низ спрайта (y+h) — это v_min.
    const float u_min = r.x / tex_w;
    const float u_max = (r.x + r.w) / tex_w;
    const float v_min = 1.0f - (r.y + r.h) / tex_h;
    const float v_max = 1.0f - r.y / tex_h;

    return glm::vec4(u_min, v_min, u_max, v_max);
}

bool Texture_Atlas::get_sprite_pixel_rect(const std::string& sprite_name, glm::vec4& out_rect) const {
    const auto it = m_sprites.find(sprite_name);
    if (it == m_sprites.end()) return false;
    out_rect = glm::vec4(it->second.x, it->second.y, it->second.w, it->second.h);
    return true;
}
