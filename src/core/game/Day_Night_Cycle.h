#ifndef OPTICRAFT_DAY_NIGHT_CYCLE_H
#define OPTICRAFT_DAY_NIGHT_CYCLE_H

#include <glm/glm.hpp>

// Держит "время суток" и всё, что из него выводится: направление на солнце/луну (для
// рендера двух billboard-дисков в небе), цвет неба и общий множитель яркости сцены
// (ambient — им умножается уже существующая per-face подсветка в фрагментных шейдерах
// чанков, см. Renderer::render_chunks).
//
// Не привязано к Chunk_Manager/Chunk — чисто вычисление по времени, ничего не трогает
// в мире, поэтому безопасно обновлять с главного потока как обычный игровой объект.
class Day_Night_Cycle {
public:
    // full_cycle_seconds: длительность ПОЛНОГО круга (день + ночь), по умолчанию 20 минут,
    // как и попросили — не 20 минут дня И 20 минут ночи отдельно.
    explicit Day_Night_Cycle(float full_cycle_seconds = 1200.0f);

    void update(float delta_time);

    // Только для debug-режима (F10, см. Game::handle_gameplay_input): сдвигает время суток
    // вперёд на произвольный интервал вне обычного хода часов.
    void advance(float seconds);

    // Прямая установка времени суток (команда /time set). t в [0,1): 0 — рассвет, 0.25 —
    // полдень, 0.5 — закат, 0.75 — полночь. Значения вне [0,1) заворачиваются по модулю.
    void set_normalized_time(float t);

    // [0, 1) — 0 — рассвет (солнце у горизонта, восходит), 0.25 — примерно полдень (солнце
    // в зените), 0.5 — закат, 0.75 — полночь (луна в зените).
    float get_normalized_time() const;

    // Единичные векторы направления НА солнце/луну от игрока — используются и для позиции
    // billboard-диска в небе (camera_pos + direction * distance), и для будущей затенённости
    // (сейчас не используется, но направление уже честное "как в реальности").
    glm::vec3 get_sun_direction() const;
    glm::vec3 get_moon_direction() const { return -get_sun_direction(); } // всегда напротив солнца

    // Плавный переход между цветом неба днём и ночью (без резкого рассвета/заката —
    // это уже осознанное упрощение первой версии, см. комментарий о длительности цикла в `Config.h`).
    glm::vec3 get_sky_color() const;

    // Множитель на существующую per-face яркость в шейдерах чанков (день ~1.0, ночь тусклее,
    // но не абсолютный чёрный — на голой ambient-модели без реального distant-shadow это
    // выглядело бы некрасиво "выключенным").
    float get_ambient_intensity() const;

private:
    float m_full_cycle_seconds;
    float m_time_seconds = 0.0f; // [0, m_full_cycle_seconds)

    // sin(elevation_angle) направления на солнце — вынесено отдельно, чтобы не пересчитывать
    // синус трижды в get_sky_color/get_ambient_intensity/get_sun_direction за один кадр.
    float sun_elevation() const;
};

#endif // OPTICRAFT_DAY_NIGHT_CYCLE_H
