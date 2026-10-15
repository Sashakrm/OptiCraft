//
// Mob — экземпляр моба в мире: transform + текущее состояние анимации.
// Сама геометрия/текстуры/скелет живут в Mob_Model и переиспользуются
// между всеми Mob одного типа (модель грузится с диска один раз,
// см. Mob::load — кэш по пути к файлу).
//
// Пример использования:
//
//   Mob zombie;
//   zombie.load("assets/mobs/zombie.glb");
//   zombie.set_position({10.0f, 65.0f, 10.0f});
//   zombie.play_animation("Walk");
//   ...
//   // каждый кадр:
//   zombie.update(delta_time);
//   zombie.render(mob_shader, view, projection);
//
#ifndef OPTICRAFT_MOB_H
#define OPTICRAFT_MOB_H

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include "mobs/Mob_Model.h"
#include "rendering/Shader.h"

class Mob {
public:
    Mob() = default;

    // Загружает (или берёт из общего кэша, если уже загружалась) модель
    // из .glb по пути. Возвращает false, если файл не найден/битый.
    bool load(const std::string& glb_path);

    // Двигает время текущего анимационного клипа. Ничего не делает, если
    // анимация не задана (play_animation ещё не вызывался) или модель без скелета.
    void update(float delta_time);

    // Переключает проигрываемый клип по имени (= имя Action'а в Blender).
    // loop=false — для одноразовых анимаций (Attack, Death): по достижении
    // конца клип останавливается на последнем кадре, см. is_animation_finished().
    // Повторный вызов с уже проигрываемым именем — no-op (не сбрасывает время).
    void play_animation(const std::string& name, bool loop = true);
    const std::string& get_current_animation() const { return m_current_animation; }
    bool is_animation_finished() const;
    // Есть ли в загруженной модели клип с таким именем — проверять перед
    // play_animation(), чтобы не звать её вхолостую на моделях без анимаций
    // (play_animation в этом случае каждый раз пишет в лог warning).
    bool has_animation(const std::string& name) const { return m_model && m_model->has_animation(name); }

    void render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient_intensity) const;

    void set_position(const glm::vec3& position) { m_position = position; }
    const glm::vec3& get_position() const { return m_position; }
    void set_yaw_degrees(float yaw) { m_yaw_degrees = yaw; }
    float get_yaw_degrees() const { return m_yaw_degrees; }
    float get_scale() const { return m_scale; }
    void set_scale(float scale) { m_scale = scale; }
    // Крен вокруг локальной оси Z (падение на бок при смерти) вокруг точки на высоте
    // roll_pivot_y над ногами — чтобы упавший моб лежал на земле, а не проваливался в неё.
    void set_roll_degrees(float roll) { m_roll_degrees = roll; }
    void set_roll_pivot_height(float pivot_y) { m_roll_pivot_y = pivot_y; }
    // Красная вспышка при уроне: 0 = обычный цвет, 1 = максимально красный.
    void set_hurt_flash(float amount) { m_hurt_flash = amount; }

    bool is_loaded() const { return m_model && m_model->is_loaded(); }

    // Габариты модели в её локальных координатах (до поворота по yaw, до масштаба), посчитанные
    // по РЕАЛЬНОЙ геометрии со скиннингом в bind-позе — ровно то, что рисует шейдер. Возвращает
    // false, если модель не загружена или геометрия пустая. Нужно для хитбоксов: размеры из
    // Config (pig_width/pig_height) описывают коллизию, а не то, во что игрок целится глазами.
    bool get_local_bounds(glm::vec3& out_min, glm::vec3& out_max) const;

    // Чистит статический кэш моделей (см. Mob::load) — ОБЯЗАТЕЛЬНО вызвать
    // ДО разрушения GL-контекста при завершении игры (Game::shutdown(),
    // перед m_window_ptr.reset()). Кэш — static-переменная внутри load(),
    // у неё static storage duration: без явной очистки она живёт до конца
    // процесса и держит свои shared_ptr<Mob_Model> дольше, чем сами Mob —
    // очистка одних только Mob (например Mobs_Update::m_pigs) НЕ освобождает
    // модель, пока жив кэш. Столкнулись с этим на практике: без вызова
    // деструктор Mob_Model отрабатывал в exit-хендлерах ПОСЛЕ
    // glfwTerminate() и падал на glDeleteVertexArrays с мёртвым контекстом.
    static void clear_model_cache();

    // Скрыть кости, в имени которых есть token (без учёта регистра), вместе со всеми их потомками.
    // Скрытая кость схлопывается в точку сустава, поэтому её геометрия не рисуется. Нужно для
    // вида от первого лица: голова модели игрока не должна загораживать камеру. Пустая строка — выключить.
    void set_hidden_bones_matching(const std::string& token) { m_hidden_bone_token = token; }

private:
    std::string m_hidden_bone_token;
    std::shared_ptr<Mob_Model> m_model;

    glm::vec3 m_position{0.0f};
    float m_yaw_degrees = 0.0f;
    float m_scale = 1.0f;
    float m_roll_degrees = 0.0f;
    float m_roll_pivot_y = 0.3f;
    float m_hurt_flash = 0.0f;

    std::string m_current_animation;
    float m_animation_time = 0.0f;
    bool m_loop = true;

    // Считает итоговые skin-матрицы (по одной на кость, в порядке m_model->get_bones())
    // для текущего m_current_animation/m_animation_time — готовые к заливке
    // в uniform mat4 bones[] шейдера.
    void compute_bone_matrices(std::vector<glm::mat4>& out_skin_matrices, const Animation_Clip* clip, float time) const;
    void compute_bone_matrices(std::vector<glm::mat4>& out_skin_matrices) const;
};

#endif //OPTICRAFT_MOB_H
