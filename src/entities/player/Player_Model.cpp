#include "entities/player/Player_Model.h"
#include "utils/logging/Logger.h"

#include <filesystem>

namespace {
    const char* k_model_path = "assets/mobs/player.glb";
}

void Player_Model::ensure_loaded() {
    if (m_tried) return;
    m_tried = true;
    if (!std::filesystem::exists(k_model_path)) {
        // Модели нет — это не ошибка: в инвентаре останется плоский силуэт.
        Logger::info(std::string("Player_Model: ") + k_model_path + " not found, using 2D preview");
        return;
    }
    m_loaded = m_mob.load(k_model_path);
    if (!m_loaded) {
        Logger::warning(std::string("Player_Model: failed to load ") + k_model_path);
        return;
    }
    // Голова (и её дочерние кости) скрыта в виде от первого лица.
    m_mob.set_hidden_bones_matching("head");
    if (m_mob.has_animation("Idle")) m_mob.play_animation("Idle");
}

void Player_Model::set_walking(bool walking) {
    m_walking = walking;
    const char* clip = walking ? "Walk" : "Idle";
    if (m_mob.has_animation(clip)) m_mob.play_animation(clip);
}

void Player_Model::update(float delta_time, const glm::vec3& feet, float yaw_degrees, bool moving) {
    if (!m_loaded) return;
    if (moving != m_walking) set_walking(moving);
    m_mob.set_position(feet);
    m_mob.set_yaw_degrees(yaw_degrees);
    m_mob.update(delta_time);
}

void Player_Model::render_world(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient) const {
    if (!m_loaded) return;
    m_mob.render(shader, view, projection, ambient);
}

void Player_Model::render_preview(const Shader& shader, const glm::mat4& view, const glm::mat4& projection,
                                  float ambient, float yaw_degrees) {
    if (!m_loaded) return;
    // В превью голова видна: отдельная копия не нужна, поэтому скрытие снимаем на время рисования.
    // Mob::set_hidden_bones_matching принимает пустую строку — это выключает скрытие.
    m_mob.set_hidden_bones_matching("");
    m_mob.set_position(glm::vec3(0.0f));
    m_mob.set_yaw_degrees(yaw_degrees);
    m_mob.render(shader, view, projection, ambient);
    m_mob.set_hidden_bones_matching("head");
}
