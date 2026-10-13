#include "mobs/Mob.h"
#include "utils/logging/Logger.h"
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>
#include <cmath>

namespace {

glm::vec3 sample_vec3(const std::vector<Anim_Key<glm::vec3>>& keys, float time, const glm::vec3& fallback) {
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;

    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (time >= keys[i].time && time <= keys[i + 1].time) {
            const float span = keys[i + 1].time - keys[i].time;
            const float t = span > 0.0001f ? (time - keys[i].time) / span : 0.0f;
            return glm::mix(keys[i].value, keys[i + 1].value, t);
        }
    }
    return keys.back().value;
}

glm::quat sample_quat(const std::vector<Anim_Key<glm::quat>>& keys, float time, const glm::quat& fallback) {
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;

    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (time >= keys[i].time && time <= keys[i + 1].time) {
            const float span = keys[i + 1].time - keys[i].time;
            const float t = span > 0.0001f ? (time - keys[i].time) / span : 0.0f;
            return glm::slerp(keys[i].value, keys[i + 1].value, t);
        }
    }
    return keys.back().value;
}

} // namespace

namespace {
// static locals имеют static storage duration — живут до конца процесса,
// если их явно не почистить (см. Mob::clear_model_cache и предупреждение в Mob.h).
std::unordered_map<std::string, std::shared_ptr<Mob_Model>>& model_cache() {
    static std::unordered_map<std::string, std::shared_ptr<Mob_Model>> cache;
    return cache;
}
} // namespace

bool Mob::load(const std::string& glb_path) {
    // Общий кэш моделей по пути к файлу: у 50 зомби на карте — одна Mob_Model
    // и одни GPU-буферы на всех, у каждого Mob своё только состояние анимации.
    auto& cache = model_cache();

    if (const auto it = cache.find(glb_path); it != cache.end()) {
        m_model = it->second;
        return m_model->is_loaded();
    }

    auto model = std::make_shared<Mob_Model>();
    if (!model->load_from_file(glb_path)) {
        return false;
    }
    cache[glb_path] = model;
    m_model = model;
    return true;
}

void Mob::clear_model_cache() {
    model_cache().clear();
}

void Mob::play_animation(const std::string& name, bool loop) {
    if (m_current_animation == name) return; // уже играет — не сбрасываем время

    if (!m_model || !m_model->has_animation(name)) {
        LOG_WARN("Mob::play_animation: animation '" + name + "' not found");
        return;
    }

    m_current_animation = name;
    m_animation_time = 0.0f;
    m_loop = loop;
}

bool Mob::is_animation_finished() const {
    if (m_loop || m_current_animation.empty() || !m_model) return false;
    const Animation_Clip* clip = m_model->find_animation(m_current_animation);
    return clip && m_animation_time >= clip->duration;
}

void Mob::update(float delta_time) {
    if (!m_model || m_current_animation.empty()) return;

    const Animation_Clip* clip = m_model->find_animation(m_current_animation);
    if (!clip || clip->duration <= 0.0f) return;

    m_animation_time += delta_time;
    if (m_animation_time > clip->duration) {
        m_animation_time = m_loop ? std::fmod(m_animation_time, clip->duration) : clip->duration;
    }
}

void Mob::compute_bone_matrices(std::vector<glm::mat4>& out_skin_matrices) const {
    const Animation_Clip* clip = m_current_animation.empty() ? nullptr : m_model->find_animation(m_current_animation);
    compute_bone_matrices(out_skin_matrices, clip, m_animation_time);
}

void Mob::compute_bone_matrices(std::vector<glm::mat4>& out_skin_matrices, const Animation_Clip* clip, float time) const {
    const std::vector<Mob_Bone>& bones = m_model->get_bones();
    out_skin_matrices.assign(bones.size(), glm::mat4(1.0f));
    if (bones.empty()) return;

    std::vector<glm::mat4> global_matrices(bones.size(), glm::mat4(1.0f));

    for (const int bone_index : m_model->get_bone_eval_order()) {
        const Mob_Bone& bone = bones[bone_index];

        glm::vec3 translation = bone.bind_translation;
        glm::quat rotation = bone.bind_rotation;
        glm::vec3 scale = bone.bind_scale;

        if (clip) {
            for (const Bone_Animation_Channel& channel : clip->channels) {
                if (channel.bone_index != bone_index) continue;
                if (!channel.translation_keys.empty()) translation = sample_vec3(channel.translation_keys, time, translation);
                if (!channel.rotation_keys.empty()) rotation = sample_quat(channel.rotation_keys, time, rotation);
                if (!channel.scale_keys.empty()) scale = sample_vec3(channel.scale_keys, time, scale);
                break;
            }
        }

        const glm::mat4 local_matrix = glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
        // Для корневых костей — не identity, а полная трансформация предков
        // выше skin.joints (см. Mob_Bone::root_parent_transform). Для
        // остальных — обычное наследование через уже посчитанного родителя.
        const glm::mat4 parent_global = bone.parent_index >= 0 ? global_matrices[bone.parent_index] : bone.root_parent_transform;

        global_matrices[bone_index] = parent_global * local_matrix;
        out_skin_matrices[bone_index] = global_matrices[bone_index] * bone.inverse_bind_matrix;
    }
}

void Mob::render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const {
    if (!is_loaded()) return;

    std::vector<glm::mat4> skin_matrices;
    compute_bone_matrices(skin_matrices);
    const bool has_skin = !skin_matrices.empty();
    if (!has_skin) skin_matrices.assign(1, glm::mat4(1.0f)); // шейдер всё равно ждёт непустой массив

    glm::mat4 model_matrix(1.0f);
    model_matrix = glm::translate(model_matrix, m_position);
    model_matrix = glm::rotate(model_matrix, glm::radians(m_yaw_degrees), glm::vec3(0.0f, 1.0f, 0.0f));
    if (m_roll_degrees != 0.0f) {
        model_matrix = glm::translate(model_matrix, glm::vec3(0.0f, m_roll_pivot_y, 0.0f));
        model_matrix = glm::rotate(model_matrix, glm::radians(m_roll_degrees), glm::vec3(0.0f, 0.0f, 1.0f));
        model_matrix = glm::translate(model_matrix, glm::vec3(0.0f, -m_roll_pivot_y, 0.0f));
    }
    model_matrix = glm::scale(model_matrix, glm::vec3(m_scale));

    shader.use();
    shader.set_mat4("model", model_matrix);
    shader.set_mat4("view", view);
    shader.set_mat4("projection", projection);
    shader.set_int("has_skin", has_skin ? 1 : 0);
    shader.set_mat4_array("bones", skin_matrices);
    shader.set_float("ambient", ambient_intensity);
    shader.set_float("hurt_flash", m_hurt_flash);

    if (Texture* albedo = m_model->get_texture("albedo")) {
        albedo->bind(0);
        shader.set_int("albedo_texture", 0);
    }

    glBindVertexArray(m_model->get_vao());
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_model->get_index_count()), GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

bool Mob::get_local_bounds(glm::vec3& out_min, glm::vec3& out_max) const {
    if (!is_loaded()) return false;

    if (!m_model->has_bounds()) {
        const std::vector<Mob_Vertex>& vertices = m_model->get_cpu_vertices();
        if (vertices.empty()) return false;

        // Габариты берём в позе Idle в t=0 (если такого клипа нет — в bind-позе): в ней моб стоит
        // ровно, а размах ног/головы при ходьбе хитбокс не раздувает.
        const Animation_Clip* pose = m_model->find_animation("Idle");
        std::vector<glm::mat4> skin;
        compute_bone_matrices(skin, pose, 0.0f);
        const bool has_skin = !skin.empty();

        // Защита от битых моделей: вершины, которые скиннинг унёс дальше k_max_extent блоков от
        // начала координат моба (сломанный экспорт FBX->glTF, см. предупреждения «bind-pose
        // sanity check» при загрузке), в габариты не входят — иначе хитбокс стал бы огромным.
        constexpr float k_max_extent = 8.0f;
        int used_vertices = 0;
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const Mob_Vertex& v : vertices) {
            glm::vec4 p(v.position, 1.0f);
            if (has_skin) {
                glm::mat4 m(0.0f);
                for (int i = 0; i < 4; ++i) {
                    const float w = v.joint_weights[i];
                    const int j = v.joint_indices[i];
                    if (w > 0.0f && j >= 0 && j < static_cast<int>(skin.size())) m += skin[j] * w;
                }
                p = m * p;
            }
            const glm::vec3 q(p);
            if (std::abs(q.x) > k_max_extent || std::abs(q.y) > k_max_extent || std::abs(q.z) > k_max_extent) continue;
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
            ++used_vertices;
        }
        if (used_vertices < 8) return false;
        if (!(lo.x < hi.x && lo.y < hi.y && lo.z < hi.z)) return false;
        m_model->set_bounds(lo, hi);
        LOG_INFO("Mob: model bounds min(" + std::to_string(lo.x) + ", " + std::to_string(lo.y) + ", " + std::to_string(lo.z) +
                 ") max(" + std::to_string(hi.x) + ", " + std::to_string(hi.y) + ", " + std::to_string(hi.z) + ")");
    }

    out_min = m_model->get_bounds_min();
    out_max = m_model->get_bounds_max();
    return true;
}
