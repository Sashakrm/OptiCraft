//
// Mob_Model — загрузка скелетных моделей мобов из .glb (glTF Binary).
//
// Идея: в Blender один раз договариваемся о конвенции, дальше ЛЮБОЙ .glb,
// сделанный по этой конвенции, грузится этой системой без единой правки
// кода под конкретного моба:
//
//   - Кости скелета — обычный армейтур Blender, имена произвольные (мы не
//     завязываемся на конкретные имена костей — работаем с готовой
//     иерархией и inverse bind matrices, которые экспортирует glTF).
//   - Анимации — каждый Action в Blender становится отдельным именованным
//     animation-клипом в glTF. Имя клипа = имя Action'а. Например "Idle",
//     "Walk", "Attack_01", "Death" — эти имена потом используются в коде
//     через Mob::play_animation("Walk"), без ручной привязки кейфреймов.
//   - Материалы — базовая (albedo) и normal-текстуры подтягиваются
//     автоматически из тех слотов material'а, куда их подключили в Blender
//     (Base Color / Normal в Principled BSDF). Работает и если текстуры
//     встроены в .glb (Pack Textures при экспорте), и если лежат рядом
//     отдельными файлами при экспорте в .gltf.
//
// Экспорт из Blender: File > Export > glTF 2.0 (.glb/.gltf), формат .glb,
// галки "Export Animations" и "Export Skinning" обязательны.

#ifndef OPTICRAFT_MOB_MODEL_H
#define OPTICRAFT_MOB_MODEL_H

#include <glad/gl.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include "rendering/atlas/Texture.h"

// Максимум костей на одного моба — ограничение размера uniform-массива в
// шейдере (см. assets/shaders/mob_vertex.glsl, bones[MAX_MOB_BONES]).
// Для типичного гуманоида/животного за глаза хватает 64.
constexpr int MAX_MOB_BONES = 64;

// Одна кость скелета. Индекс кости в Mob_Model::bones — это и есть её "id",
// используемый в joint_indices вершин и в channels анимаций.
struct Mob_Bone {
    std::string name;
    int parent_index = -1; // -1 = корень скелета

    // Bind-поза (T-поза/A-поза из Blender) — используется как значение по
    // умолчанию для костей, не затронутых текущим анимационным клипом.
    glm::vec3 bind_translation{0.0f};
    glm::quat bind_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 bind_scale{1.0f};

    // Переводит вершину из локального пространства кости в pose space меша.
    // Экспортируется glTF готовым (skin.inverseBindMatrices).
    glm::mat4 inverse_bind_matrix{1.0f};

    // Только для корневых костей скелета (parent_index == -1): полная
    // трансформация ВСЕХ предков этой кости в исходном node-графе glTF
    // выше самого верхнего joint'а (Armature/RootNode/масштабирующие
    // обёртки конвертера FBX→glTF и т.п.). Для остальных костей — identity,
    // не используется (наследуется через parent_index обычным образом).
    //
    // Почему это нужно: собственный transform mesh-узла в формуле
    // скиннинга АЛГЕБРАИЧЕСКИ СОКРАЩАЕТСЯ (worldPos = jointGlobalTransform *
    // inverseBindMatrix * localPos — transform самого меша в этой формуле
    // не участвует вообще). Значит jointGlobalTransform обязан быть
    // трансформацией кости от ИСТИННОГО корня сцены, а не только в
    // пределах skin.joints. Если экспортёр (Sketchfab/FBX→glTF и т.п.)
    // держит масштаб/разворот осей на нодах ВЫШЕ корневого joint'а —
    // без этого поля они теряются и модель выходит перекошенной/гигантской
    // (поймано на practice на minecraft_pig.glb — см. чат).
    glm::mat4 root_parent_transform{1.0f};
};

template <typename T>
struct Anim_Key {
    float time = 0.0f;
    T value{};
};

// Кейфреймы одной кости внутри одного анимационного клипа. Канал может
// присутствовать не для всех TRS-компонент сразу (например, только
// rotation) — тогда остальные компоненты берутся из bind-позы кости.
struct Bone_Animation_Channel {
    int bone_index = -1;
    std::vector<Anim_Key<glm::vec3>> translation_keys;
    std::vector<Anim_Key<glm::quat>> rotation_keys;
    std::vector<Anim_Key<glm::vec3>> scale_keys;
};

// Один Action из Blender (Idle/Walk/Attack_01/...).
struct Animation_Clip {
    std::string name;
    float duration = 0.0f; // секунды
    std::vector<Bone_Animation_Channel> channels;
};

// Вершина скиннингованного меша — позиция, нормаль, UV + до 4 костей с весами.
struct Mob_Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec2 uv{0.0f};
    glm::ivec4 joint_indices{0};
    glm::vec4 joint_weights{0.0f};
};

// Загруженная модель моба: геометрия на GPU + скелет + анимации + текстуры.
// Одну загруженную Mob_Model можно (и нужно) переиспользовать для многих
// экземпляров Mob в мире — сама геометрия/текстуры на GPU одни на всех,
// у каждого Mob своё только текущее состояние анимации и transform.
// ВАЖНО про время жизни: деструктор освобождает GL-ресурсы (VAO/VBO/EBO),
// поэтому любой Mob_Model (и любой Mob, который его держит) должен быть
// уничтожен ДО разрушения GL-контекста (Window). Если у Game/контейнера
// с мобами объявление идёт раньше Window по членам класса — порядок
// автоматически правильный (члены рушатся в обратном порядке объявления).
// Проверено на практике: если этот порядок нарушить, деструктор падает
// в glDeleteVertexArrays на мёртвом контексте (segfault).
class Mob_Model {
public:
    Mob_Model() = default;
    ~Mob_Model();

    Mob_Model(const Mob_Model&) = delete;
    Mob_Model& operator=(const Mob_Model&) = delete;

    // Загружает модель из .glb (или .gltf — если рядом лежат .bin/текстуры).
    bool load_from_file(const std::string& path);

    bool is_loaded() const { return m_is_loaded; }

    const std::vector<Mob_Bone>& get_bones() const { return m_bones; }
    // Порядок обхода костей, в котором родитель гарантированно идёт раньше
    // потомка (BFS от корня/корней скелета). Порядок хранения m_bones
    // совпадает с skin.joints из glTF и НЕ гарантированно топологический,
    // поэтому вычисление глобальных матриц (см. Mob::compute_bone_matrices)
    // должно идти именно в этом порядке, а не по индексу.
    const std::vector<int>& get_bone_eval_order() const { return m_bone_eval_order; }
    const Animation_Clip* find_animation(const std::string& name) const;
    bool has_animation(const std::string& name) const { return find_animation(name) != nullptr; }

    Texture* get_texture(const std::string& role) const;

    // Вершины, оставленные в оперативной памяти после загрузки на GPU (у свиньи их немного) —
    // нужны для расчёта реальных габаритов модели, см. Mob::get_local_bounds().
    const std::vector<Mob_Vertex>& get_cpu_vertices() const { return m_cpu_vertices; }
    // Кэш габаритов в bind-позе (ось Y от ног). Считается один раз на модель.
    bool has_bounds() const { return m_has_bounds; }
    void set_bounds(const glm::vec3& min_corner, const glm::vec3& max_corner) {
        m_bounds_min = min_corner; m_bounds_max = max_corner; m_has_bounds = true;
    }
    const glm::vec3& get_bounds_min() const { return m_bounds_min; }
    const glm::vec3& get_bounds_max() const { return m_bounds_max; }

    unsigned int get_vao() const { return m_vao; }
    size_t get_index_count() const { return m_index_count; }

private:
    bool m_is_loaded = false;

    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;
    size_t m_index_count = 0;

    std::vector<Mob_Vertex> m_cpu_vertices;
    bool m_has_bounds = false;
    glm::vec3 m_bounds_min{0.0f};
    glm::vec3 m_bounds_max{0.0f};

    std::vector<Mob_Bone> m_bones;
    std::vector<int> m_bone_eval_order;
    std::unordered_map<std::string, int> m_bone_name_to_index;
    std::unordered_map<std::string, Animation_Clip> m_animations;

    // Ключ — роль текстуры: "albedo", "normal". См. Mob_Model.cpp:
    // роль берётся из того, к какому слоту Principled BSDF подключена
    // текстура в материале, а не из имени файла — так конвенция по
    // именованию файлов текстур в Blender не нужна вообще.
    std::unordered_map<std::string, std::unique_ptr<Texture>> m_textures;

    void upload_mesh(const std::vector<Mob_Vertex>& vertices, const std::vector<uint32_t>& indices);
};

#endif //OPTICRAFT_MOB_MODEL_H
