//
// Player_Model — 3D-модель самого игрока: видна в инвентаре (превью) и в виде от первого лица,
// где при взгляде вниз видны ноги и тело. Голова скрывается, чтобы не загораживать камеру.
//
// Модель: assets/mobs/player.glb (та же конвенция, что у остальных мобов, см. Mob_Model).
// Если файла нет — модель не грузится, игра работает как раньше (в инвентаре остаётся плоский
// силуэт), ошибки нет. Ожидаемые клипы: Idle и Walk; их отсутствие не ломает модель.
//
// Компромисс: модель стоит в мире ровно там, где ноги игрока, и камера находится внутри её
// тела на уровне глаз. Это значит, что при взгляде вниз торс может загораживать обзор — так
// и задумано (ноги видны), но точную высоту головы и поворот стоит подобрать вживую.
//
#ifndef OPTICRAFT_PLAYER_MODEL_H
#define OPTICRAFT_PLAYER_MODEL_H

#include <glm/glm.hpp>
#include "mobs/Mob.h"

class Shader;

class Player_Model {
public:
    // Пытается загрузить модель один раз. Повторные вызовы ничего не делают.
    void ensure_loaded();
    bool is_loaded() const { return m_loaded; }

    // feet — ноги игрока; yaw — куда смотрит (градусы); moving — идёт ли игрок (меняет клип).
    void update(float delta_time, const glm::vec3& feet, float yaw_degrees, bool moving);

    // Вид от первого лица: модель в мире, голова скрыта.
    void render_world(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient) const;

    // Превью в инвентаре: своя камера и свой поворот (yaw), голова видна.
    void render_preview(const Shader& shader, const glm::mat4& view, const glm::mat4& projection,
                        float ambient, float yaw_degrees);

private:
    Mob m_mob;
    bool m_tried = false;
    bool m_loaded = false;
    bool m_walking = false;
    float m_preview_time = 0.0f;

    void set_walking(bool walking);
};

#endif //OPTICRAFT_PLAYER_MODEL_H
