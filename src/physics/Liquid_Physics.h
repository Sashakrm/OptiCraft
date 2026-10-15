//
// Liquid_Physics — общая физика тел в жидкости для игрока и всех мобов.
//
// Зачем он нужен. Раньше вода была только картинкой: вода растекалась (Fluid_Simulator), но
// ни игрок, ни моб её не замечали. Теперь каждое тело перед своим шагом спрашивает этот модуль:
//   * sample()  — в воде ли тело, под водой ли голова и куда его сносит течение;
//   * Breath    — сколько воздуха осталось и сколько урона утопления набежало за шаг.
// Сами тела решают, что делать с этим (тонуть, плыть, замедляться): у каждого вида моба флаг
// can_swim свой, см. Hostile_Stats::can_swim и Passive_Stats::can_swim.
//
// Модель. Жидкость — блок Liquid из blocks.json. Её уровень — в метаданных (см. Block_Meta):
// источник = 8, течение = 8 - level, падающая вода = 8. Течение тянет тело от высокого уровня к
// низкому только между соседними клетками жидкости (как в Minecraft); сухой край течение не тянет.
//
// Компромисс: плавание сделано приближённо (сглаженная вертикальная скорость и множитель
// горизонтальной скорости), а не настоящей гидродинамикой. Этого достаточно для игры и дёшево.
//
#ifndef OPTICRAFT_LIQUID_PHYSICS_H
#define OPTICRAFT_LIQUID_PHYSICS_H

#include <glm/glm.hpp>

#include "utils/Config.h"

class Chunk_Manager;

namespace Liquid_Physics {

    // Что тело видит в своей точке прямо сейчас.
    struct Sample {
        bool body_in_liquid = false;  // центр тела в жидкости: тормозит, плавает или тонет
        bool head_in_liquid = false;  // голова под водой: тратится воздух
        glm::vec3 flow{0.0f};         // течение, блоков/с (только горизонталь)
    };

    // feet — ноги тела (низ коллизии), height — его высота.
    Sample sample(const Chunk_Manager& chunks, const glm::vec3& feet, float height);

    // Вертикальная скорость тела в жидкости на этом шаге.
    //  * can_swim — по умолчанию удерживает высоту; wants_up поднимает, wants_down погружает.
    //    sink_when_idle включает для игрока медленное погружение без гребков.
    //  * не can_swim — тонет, скорость падения ограничена water_sink_speed.
    // Вызывать только если body_in_liquid. Возвращает новую vertical_velocity.
    float swim_vertical_velocity(const Sample& s, bool can_swim, bool wants_up, float vertical_velocity, float dt,
                                 bool wants_down = false, bool sink_when_idle = false);

    // Множитель горизонтальной скорости тела в жидкости (1.0 вне воды).
    float horizontal_drag(const Sample& s);

    // Воздух. Пока голова под водой — тратится; над водой быстро восстанавливается. Когда воздух
    // кончился — каждый drown_damage_interval возвращает 1 событие утопления (умножать на урон).
    struct Breath {
        float air = Config::breath_max_seconds; // сколько секунд воздуха осталось, 0..breath_max_seconds
        float drown_timer = 0.0f;   // сколько секунд прошло с последнего удара утопления

        // Возвращает, сколько раз за шаг сработало утопление (обычно 0 или 1).
        int tick(float dt, bool head_in_liquid, float max_air);
        bool is_drowning() const { return air <= 0.0f; }
    };

} // namespace Liquid_Physics

#endif //OPTICRAFT_LIQUID_PHYSICS_H
