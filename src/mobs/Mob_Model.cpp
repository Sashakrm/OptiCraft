//
// Created for OptiCraft mob system.
//
// Парсер .glb/.gltf на cgltf (https://github.com/jkuhlmann/cgltf, лежит
// в external/cgltf/). Здесь же (и только здесь, чтобы не задваивать
// реализацию) объявлен CGLTF_IMPLEMENTATION.

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define GLM_ENABLE_EXPERIMENTAL
#include "mobs/Mob_Model.h"
#include "utils/logging/Logger.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <filesystem>
#include <queue>

namespace {

// Директория файла — нужна, чтобы резолвить относительные пути на внешние
// .bin/текстуры при загрузке "рыхлого" .gltf (для .glb всё встроено, и
// это не используется).
std::string parent_directory(const std::string& path) {
    const std::filesystem::path p(path);
    return p.has_parent_path() ? p.parent_path().string() : ".";
}

std::unique_ptr<Texture> load_texture_from_gltf_image(const cgltf_texture* gltf_texture, const std::string& base_dir) {
    const cgltf_image* image = gltf_texture ? gltf_texture->image : nullptr;
    if (!image) return nullptr;

    // glTF: V идёт СВЕРХУ вниз, поэтому картинку НЕ переворачиваем (для блоков/HUD Texture
    // переворачивает по умолчанию — там V вверх). Фильтр берём из sampler'а файла:
    // magFilter 9728 = NEAREST (пиксель-арт, как у майнкрафтовской свиньи).
    constexpr int k_gl_nearest = 9728;
    const bool nearest = gltf_texture->sampler && gltf_texture->sampler->mag_filter == k_gl_nearest;

    auto texture = std::make_unique<Texture>();

    if (image->buffer_view) {
        // Текстура встроена в .glb — сырые байты PNG/JPEG лежат прямо в буфере.
        const cgltf_buffer_view* view = image->buffer_view;
        const auto* bytes = static_cast<const unsigned char*>(view->buffer->data) + view->offset;
        const std::string debug_name = image->name ? image->name : "<embedded>";
        if (!texture->load_from_memory(bytes, static_cast<int>(view->size), debug_name, /*flip_vertically=*/false, nearest)) {
            return nullptr;
        }
        return texture;
    }

    if (image->uri) {
        // Внешний файл (случай "рыхлого" .gltf) — путь относительно самого .gltf.
        std::string uri = image->uri;
        const std::string path = base_dir + "/" + uri;
        if (!texture->load_from_file(path, /*flip_vertically=*/false, nearest)) {
            return nullptr;
        }
        return texture;
    }

    return nullptr;
}

glm::vec3 read_vec3(const cgltf_accessor* accessor, cgltf_size index) {
    float v[3] = {0.0f, 0.0f, 0.0f};
    cgltf_accessor_read_float(accessor, index, v, 3);
    return {v[0], v[1], v[2]};
}

// glTF хранит кватернионы как (x, y, z, w); glm::quat конструируется как (w, x, y, z).
glm::quat read_quat(const cgltf_accessor* accessor, cgltf_size index) {
    float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cgltf_accessor_read_float(accessor, index, v, 4);
    return {v[3], v[0], v[1], v[2]};
}

// Локальный transform ноды: либо явная матрица (has_matrix), либо TRS.
// Матрица встречается у "технических" нод-обёрток (конвертеры FBX→glTF,
// оси/масштаб) — у joint'ов почти всегда TRS, но на всякий случай
// поддерживаем оба варианта для ЛЮБОЙ ноды, не только костей.
glm::mat4 read_node_local_matrix(const cgltf_node* node) {
    if (node->has_matrix) {
        return glm::make_mat4(node->matrix);
    }
    const glm::vec3 t = node->has_translation ? glm::vec3(node->translation[0], node->translation[1], node->translation[2]) : glm::vec3(0.0f);
    const glm::quat r = node->has_rotation ? glm::quat(node->rotation[3], node->rotation[0], node->rotation[1], node->rotation[2]) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::vec3 s = node->has_scale ? glm::vec3(node->scale[0], node->scale[1], node->scale[2]) : glm::vec3(1.0f);
    return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
}

// Полная трансформация ВСЕХ предков node (НЕ включая сам node), от
// истинного корня сцены вниз. См. комментарий у Mob_Bone::root_parent_transform.
glm::mat4 compute_ancestor_transform(const cgltf_node* node) {
    std::vector<const cgltf_node*> chain;
    for (const cgltf_node* cur = node->parent; cur; cur = cur->parent) chain.push_back(cur);

    glm::mat4 result(1.0f);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        result = result * read_node_local_matrix(*it);
    }
    return result;
}

} // namespace

Mob_Model::~Mob_Model() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
}

const Animation_Clip* Mob_Model::find_animation(const std::string& name) const {
    const auto it = m_animations.find(name);
    return it != m_animations.end() ? &it->second : nullptr;
}

Texture* Mob_Model::get_texture(const std::string& role) const {
    const auto it = m_textures.find(role);
    return it != m_textures.end() ? it->second.get() : nullptr;
}

bool Mob_Model::load_from_file(const std::string& path) {
    cgltf_options options{};
    cgltf_data* data = nullptr;

    cgltf_result result = cgltf_parse_file(&options, path.c_str(), &data);
    if (result != cgltf_result_success) {
        LOG_ERROR("Mob_Model: cgltf_parse_file failed for " + path + " (code " + std::to_string(static_cast<int>(result)) + ")");
        return false;
    }

    result = cgltf_load_buffers(&options, data, path.c_str());
    if (result != cgltf_result_success) {
        LOG_ERROR("Mob_Model: cgltf_load_buffers failed for " + path + " (code " + std::to_string(static_cast<int>(result)) + ")");
        cgltf_free(data);
        return false;
    }

    if (data->meshes_count == 0) {
        LOG_ERROR("Mob_Model: no meshes in " + path);
        cgltf_free(data);
        return false;
    }
    // ---- Скелет ----
    // Индекс кости = индекс в skin.joints — это же соглашение, в котором
    // экспортированы JOINTS_0 у вершин, так что переиндексировать не нужно.
    std::unordered_map<const cgltf_node*, int> node_to_bone_index;
    if (data->skins_count > 0) {
        const cgltf_skin& skin = data->skins[0];
        m_bones.resize(skin.joints_count);

        for (cgltf_size i = 0; i < skin.joints_count; ++i) {
            node_to_bone_index[skin.joints[i]] = static_cast<int>(i);
        }

        for (cgltf_size i = 0; i < skin.joints_count; ++i) {
            const cgltf_node* node = skin.joints[i];
            Mob_Bone& bone = m_bones[i];
            bone.name = node->name ? node->name : ("bone_" + std::to_string(i));
            m_bone_name_to_index[bone.name] = static_cast<int>(i);

            const auto parent_it = node->parent ? node_to_bone_index.find(node->parent) : node_to_bone_index.end();
            bone.parent_index = (parent_it != node_to_bone_index.end()) ? parent_it->second : -1;
            if (bone.parent_index == -1) {
                // Настоящий корень этой кости в скелете, НО не обязательно
                // корень всей сцены — предки выше (Armature/масштабирующие
                // обёртки экспортёра) в skin.joints не входят, но их
                // transform всё равно нужен, см. Mob_Bone::root_parent_transform.
                bone.root_parent_transform = compute_ancestor_transform(node);
            }

            if (node->has_matrix) {
                // Редкий случай для joint'ов (обычно TRS), но на всякий случай
                // раскладываем явную матрицу на T/R/S тем же способом, что
                // и остальной код (анимации сэмплируются в T/R/S, не в матрицах).
                glm::vec3 skew; glm::vec4 perspective;
                glm::mat4 m = glm::make_mat4(node->matrix);
                glm::decompose(m, bone.bind_scale, bone.bind_rotation, bone.bind_translation, skew, perspective);
                bone.bind_rotation = glm::conjugate(bone.bind_rotation); // glm::decompose отдаёт кватернион в обратном порядке умножения
            } else {
                bone.bind_translation = node->has_translation ? glm::vec3(node->translation[0], node->translation[1], node->translation[2]) : glm::vec3(0.0f);
                bone.bind_rotation = node->has_rotation ? glm::quat(node->rotation[3], node->rotation[0], node->rotation[1], node->rotation[2]) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                bone.bind_scale = node->has_scale ? glm::vec3(node->scale[0], node->scale[1], node->scale[2]) : glm::vec3(1.0f);
            }

            if (skin.inverse_bind_matrices) {
                float m[16];
                cgltf_accessor_read_float(skin.inverse_bind_matrices, i, m, 16);
                bone.inverse_bind_matrix = glm::make_mat4(m);
            }
        }

        // BFS от всех корней (parent_index == -1) — гарантирует, что в
        // m_bone_eval_order родитель всегда встречается раньше потомка,
        // независимо от порядка skin.joints в исходном файле.
        {
            std::vector<std::vector<int>> children(m_bones.size());
            std::vector<int> roots;
            for (size_t i = 0; i < m_bones.size(); ++i) {
                if (m_bones[i].parent_index >= 0) children[m_bones[i].parent_index].push_back(static_cast<int>(i));
                else roots.push_back(static_cast<int>(i));
            }
            std::queue<int> queue;
            for (int root : roots) queue.push(root);
            while (!queue.empty()) {
                const int current = queue.front();
                queue.pop();
                m_bone_eval_order.push_back(current);
                for (int child : children[current]) queue.push(child);
            }
        }

        if (m_bones.size() > static_cast<size_t>(MAX_MOB_BONES)) {
            LOG_WARN("Mob_Model: " + path + " has " + std::to_string(m_bones.size()) +
                     " bones, more than MAX_MOB_BONES=" + std::to_string(MAX_MOB_BONES) +
                     " — extra bones will not animate correctly. Raise MAX_MOB_BONES in Mob_Model.h.");
        }

        // Sanity-check bind-позы: по определению inverseBindMatrix — это ТОЧНО
        // inverse(глобальный transform кости в bind-позе), поэтому
        // global_bind(bone) * inverse_bind_matrix(bone) обязана быть identity.
        // Если нет — TRS костей в файле не согласуется с их же
        // inverseBindMatrices (встречается у моделей из дешёвых
        // FBX→glTF конвертеров — сама модель после этого рендерится
        // перекошенной/гигантской независимо от корректности загрузчика).
        // Отдельно от MAX_MOB_BONES — это про ДАННЫЕ файла, а не про движок.
        {
            std::vector<glm::mat4> bind_global(m_bones.size(), glm::mat4(1.0f));
            for (const int bone_index : m_bone_eval_order) {
                const Mob_Bone& bone = m_bones[bone_index];
                const glm::mat4 local = glm::translate(glm::mat4(1.0f), bone.bind_translation) * glm::mat4_cast(bone.bind_rotation) * glm::scale(glm::mat4(1.0f), bone.bind_scale);
                const glm::mat4 parent = bone.parent_index >= 0 ? bind_global[bone.parent_index] : bone.root_parent_transform;
                bind_global[bone_index] = parent * local;

                const glm::mat4 residual = bind_global[bone_index] * bone.inverse_bind_matrix;
                // Диагональ НЕ годится сама по себе — у чистого поворота на
                // 90° диагональ 3x3-блока далека от (1,1,1) без всякой ошибки
                // масштаба. Инвариантная к повороту проверка — длины столбцов
                // 3x3-блока: у чистого поворота (без реального масштаба) они
                // все равны 1; отклонение — это и есть настоящий лишний масштаб.
                const float col0_len = glm::length(glm::vec3(residual[0]));
                const float col1_len = glm::length(glm::vec3(residual[1]));
                const float col2_len = glm::length(glm::vec3(residual[2]));
                const float scale_error = std::abs(col0_len - 1.0f) + std::abs(col1_len - 1.0f) + std::abs(col2_len - 1.0f);
                if (scale_error > 0.05f) {
                    LOG_WARN("Mob_Model: " + path + " — bone '" + bone.name + "' fails bind-pose sanity check "
                             "(residual scale error " + std::to_string(scale_error) + ", expected ~0). "
                             "Skinning data in this file is internally inconsistent (TRS of the joint doesn't "
                             "match its own inverseBindMatrix) — common with cheap FBX->glTF converters. "
                             "Model will likely render distorted/huge regardless of loader correctness; "
                             "re-export from Blender directly (see README convention) to fix at the source.");
                }
            }
        }
    } else {
        LOG_WARN("Mob_Model: " + path + " has no skin — model will be static (no animations).");
    }

    // ---- Анимации ----
    for (cgltf_size a = 0; a < data->animations_count; ++a) {
        const cgltf_animation& src_anim = data->animations[a];
        Animation_Clip clip;
        clip.name = src_anim.name ? src_anim.name : ("Animation_" + std::to_string(a));

        for (cgltf_size c = 0; c < src_anim.channels_count; ++c) {
            const cgltf_animation_channel& src_channel = src_anim.channels[c];
            const auto bone_it = node_to_bone_index.find(src_channel.target_node);
            if (bone_it == node_to_bone_index.end()) continue; // анимируется не кость скелета — пропускаем

            const cgltf_animation_sampler* sampler = src_channel.sampler;
            const cgltf_accessor* times = sampler->input;
            const cgltf_accessor* values = sampler->output;
            const bool is_cubic = (sampler->interpolation == cgltf_interpolation_type_cubic_spline);
            // Для CUBICSPLINE на каждый ключ идёт 3 значения (in-tangent, value, out-tangent) —
            // нам нужна только середина; тангенты игнорируем и сглаживаем обычным lerp/slerp.
            const cgltf_size value_stride = is_cubic ? 3 : 1;

            // glTF хранит translation/rotation/scale одной кости отдельными каналами, а
            // Mob::compute_bone_matrices берёт для кости ПЕРВЫЙ найденный канал клипа — поэтому
            // каналы одной кости сливаем в один (раньше у кости с поворотом И смещением работал
            // только тот канал, что шёл в файле первым, а второй молча игнорировался).
            Bone_Animation_Channel channel;
            channel.bone_index = bone_it->second;
            for (auto existing = clip.channels.begin(); existing != clip.channels.end(); ++existing) {
                if (existing->bone_index == channel.bone_index) {
                    channel = std::move(*existing);
                    clip.channels.erase(existing);
                    break;
                }
            }

            for (cgltf_size k = 0; k < times->count; ++k) {
                float t = 0.0f;
                cgltf_accessor_read_float(times, k, &t, 1);
                clip.duration = std::max(clip.duration, t);

                const cgltf_size value_index = k * value_stride + (is_cubic ? 1 : 0);

                switch (src_channel.target_path) {
                    case cgltf_animation_path_type_translation:
                        channel.translation_keys.push_back({t, read_vec3(values, value_index)});
                        break;
                    case cgltf_animation_path_type_rotation:
                        channel.rotation_keys.push_back({t, read_quat(values, value_index)});
                        break;
                    case cgltf_animation_path_type_scale:
                        channel.scale_keys.push_back({t, read_vec3(values, value_index)});
                        break;
                    default:
                        break; // weights (shape keys) — не поддерживаем в этой версии
                }
            }

            if (!channel.translation_keys.empty() || !channel.rotation_keys.empty() || !channel.scale_keys.empty()) {
                clip.channels.push_back(std::move(channel));
            }
        }

        m_animations.emplace(clip.name, std::move(clip));
    }

    // ---- Меш ----
    // Модели, скачанные готовыми (Sketchfab и т.п.), почти всегда приходят НЕ одним
    // Join'нутым объектом, а отдельным mesh на каждую часть тела (тело/ноги/голова —
    // как раз случай minecraft_pig.glb: 7 мешей на один скелет). Поэтому здесь
    // собираем ВСЕ триангулированные примитивы всех мешей в один общий буфер:
    // раз они скиннятся на один и тот же skin, JOINTS_0 у них уже в одной системе
    // индексов кости, склейка — это просто конкатенация с сдвигом индексов.
    std::vector<Mob_Vertex> vertices;
    std::vector<uint32_t> indices;
    const cgltf_material* first_material = nullptr;

    for (cgltf_size mesh_i = 0; mesh_i < data->meshes_count; ++mesh_i) {
        const cgltf_mesh& mesh = data->meshes[mesh_i];
        for (cgltf_size prim_i = 0; prim_i < mesh.primitives_count; ++prim_i) {
            const cgltf_primitive& primitive = mesh.primitives[prim_i];
            if (primitive.type != cgltf_primitive_type_triangles) {
                LOG_WARN("Mob_Model: " + path + " — skipping non-triangle primitive in mesh " + std::to_string(mesh_i));
                continue;
            }

            const cgltf_accessor* position_accessor = nullptr;
            const cgltf_accessor* normal_accessor = nullptr;
            const cgltf_accessor* uv_accessor = nullptr;
            const cgltf_accessor* joints_accessor = nullptr;
            const cgltf_accessor* weights_accessor = nullptr;

            for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
                const cgltf_attribute& attr = primitive.attributes[i];
                switch (attr.type) {
                    case cgltf_attribute_type_position: position_accessor = attr.data; break;
                    case cgltf_attribute_type_normal:   normal_accessor = attr.data; break;
                    case cgltf_attribute_type_texcoord: if (!uv_accessor) uv_accessor = attr.data; break;
                    case cgltf_attribute_type_joints:   if (!joints_accessor) joints_accessor = attr.data; break;
                    case cgltf_attribute_type_weights:  if (!weights_accessor) weights_accessor = attr.data; break;
                    default: break;
                }
            }

            if (!position_accessor) {
                LOG_WARN("Mob_Model: " + path + " — mesh " + std::to_string(mesh_i) + " has no POSITION, skipped");
                continue;
            }

            const uint32_t base_vertex = static_cast<uint32_t>(vertices.size());
            const cgltf_size vertex_count = position_accessor->count;
            vertices.resize(vertices.size() + vertex_count);

            for (cgltf_size i = 0; i < vertex_count; ++i) {
                Mob_Vertex& v = vertices[base_vertex + i];
                v.position = read_vec3(position_accessor, i);
                if (normal_accessor) v.normal = read_vec3(normal_accessor, i);
                if (uv_accessor) {
                    float uv[2] = {0.0f, 0.0f};
                    cgltf_accessor_read_float(uv_accessor, i, uv, 2);
                    v.uv = {uv[0], uv[1]};
                }
                if (joints_accessor && weights_accessor) {
                    unsigned int j[4] = {0, 0, 0, 0};
                    float w[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    cgltf_accessor_read_uint(joints_accessor, i, j, 4);
                    cgltf_accessor_read_float(weights_accessor, i, w, 4);
                    v.joint_indices = {static_cast<int>(j[0]), static_cast<int>(j[1]), static_cast<int>(j[2]), static_cast<int>(j[3])};
                    v.joint_weights = {w[0], w[1], w[2], w[3]};
                }
            }

            if (primitive.indices) {
                const cgltf_size index_count = primitive.indices->count;
                indices.reserve(indices.size() + index_count);
                for (cgltf_size i = 0; i < index_count; ++i) {
                    unsigned int idx = 0;
                    cgltf_accessor_read_uint(primitive.indices, i, &idx, 1);
                    indices.push_back(base_vertex + idx);
                }
            } else {
                // Без индексного буфера — считаем вершины идущими тройками треугольников.
                indices.reserve(indices.size() + vertex_count);
                for (cgltf_size i = 0; i < vertex_count; ++i) indices.push_back(base_vertex + static_cast<uint32_t>(i));
            }

            if (!first_material && primitive.material) first_material = primitive.material;
        }
    }

    if (vertices.empty()) {
        LOG_ERROR("Mob_Model: no usable geometry found in " + path);
        cgltf_free(data);
        return false;
    }

    upload_mesh(vertices, indices);
    m_cpu_vertices = vertices;

    // ---- Текстуры ----
    // Берём материал первого примитива, у которого он есть. Мобы с разными
    // материалами на разных частях тела (кроме albedo/normal одной текстуры
    // на всё) — уже за пределами этой версии, см. README.
    if (first_material) {
        const std::string base_dir = parent_directory(path);

        if (first_material->has_pbr_metallic_roughness && first_material->pbr_metallic_roughness.base_color_texture.texture) {
            if (auto tex = load_texture_from_gltf_image(first_material->pbr_metallic_roughness.base_color_texture.texture, base_dir)) {
                m_textures["albedo"] = std::move(tex);
            }
        }
        if (first_material->normal_texture.texture) {
            if (auto tex = load_texture_from_gltf_image(first_material->normal_texture.texture, base_dir)) {
                m_textures["normal"] = std::move(tex);
            }
        }
    }
    if (!get_texture("albedo")) {
        LOG_WARN("Mob_Model: " + path + " has no base color texture — mob will render untextured (magenta fallback recommended in shader).");
    }

    cgltf_free(data);
    m_is_loaded = true;
    LOG_INFO("Mob_Model loaded: " + path + " (" + std::to_string(vertices.size()) + " verts, " +
             std::to_string(m_bones.size()) + " bones, " + std::to_string(m_animations.size()) + " animations)");
    return true;
}

void Mob_Model::upload_mesh(const std::vector<Mob_Vertex>& vertices, const std::vector<uint32_t>& indices) {
    m_index_count = indices.size();

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(Mob_Vertex)), vertices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)), indices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Mob_Vertex), reinterpret_cast<void*>(offsetof(Mob_Vertex, position)));

    glEnableVertexAttribArray(1); // normal
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Mob_Vertex), reinterpret_cast<void*>(offsetof(Mob_Vertex, normal)));

    glEnableVertexAttribArray(2); // uv
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Mob_Vertex), reinterpret_cast<void*>(offsetof(Mob_Vertex, uv)));

    glEnableVertexAttribArray(3); // joint indices (целые!)
    glVertexAttribIPointer(3, 4, GL_INT, sizeof(Mob_Vertex), reinterpret_cast<void*>(offsetof(Mob_Vertex, joint_indices)));

    glEnableVertexAttribArray(4); // joint weights
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Mob_Vertex), reinterpret_cast<void*>(offsetof(Mob_Vertex, joint_weights)));

    glBindVertexArray(0);
}
