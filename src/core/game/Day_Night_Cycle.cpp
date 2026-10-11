#include "core/game/Day_Night_Cycle.h"

#include <cmath>
#include <algorithm>

namespace {
    constexpr float PI = 3.14159265358979323846f;

    glm::vec3 lerp_color(const glm::vec3& a, const glm::vec3& b, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return a + (b - a) * t;
    }
}

Day_Night_Cycle::Day_Night_Cycle(float full_cycle_seconds)
    : m_full_cycle_seconds(full_cycle_seconds)
{}

void Day_Night_Cycle::update(float delta_time) {
    advance(delta_time);
}

void Day_Night_Cycle::advance(float seconds) {
    m_time_seconds = std::fmod(m_time_seconds + seconds, m_full_cycle_seconds);
    if (m_time_seconds < 0.0f) {
        m_time_seconds += m_full_cycle_seconds;
    }
}

void Day_Night_Cycle::set_normalized_time(float t) {
    float wrapped = std::fmod(t, 1.0f);
    if (wrapped < 0.0f) wrapped += 1.0f;
    m_time_seconds = wrapped * m_full_cycle_seconds;
}

float Day_Night_Cycle::get_normalized_time() const {
    return m_time_seconds / m_full_cycle_seconds;
}

float Day_Night_Cycle::sun_elevation() const {
    // t=0 -> рассвет (0), t=0.25 -> полдень (1), t=0.5 -> закат (0), t=0.75 -> полночь (-1).
    return std::sin(2.0f * PI * get_normalized_time());
}

glm::vec3 Day_Night_Cycle::get_sun_direction() const {
    const float t = get_normalized_time();
    const float elevation = sun_elevation();
    // Дуга слегка наклонена по Z (не идеально по одной оси) — чисто чтобы путь солнца по
    // небу не выглядел абсолютно плоским; величина наклона не принципиальна.
    const float azimuth = 2.0f * PI * t;
    return glm::normalize(glm::vec3(std::cos(azimuth), elevation, 0.35f));
}

glm::vec3 Day_Night_Cycle::get_sky_color() const {
    // Осознанное упрощение первой версии: только плавный переход между цветом неба днём
    // и ночью по высоте солнца (smoothstep) — без отдельного оранжевого тона рассвета/заката
    // поверх. Работает достаточно хорошо на глаз, а для честного рассвет/закатного неба
    // потребовался бы отдельный третий опорный цвет и более сложная кривая смешивания.
    static const glm::vec3 day_color(0.55f, 0.75f, 0.95f);
    static const glm::vec3 night_color(0.05f, 0.07f, 0.16f);

    const float elevation = sun_elevation();
    // smoothstep по высоте солнца: полностью "ночь" ниже -0.2, полностью "день" выше 0.3,
    // плавный переход между ними — это и есть рассвет/закат по ощущениям, просто без
    // отдельного цвета.
    const float t = std::clamp((elevation + 0.2f) / 0.5f, 0.0f, 1.0f);
    const float smooth_t = t * t * (3.0f - 2.0f * t);
    return lerp_color(night_color, day_color, smooth_t);
}

float Day_Night_Cycle::get_ambient_intensity() const {
    const float elevation = sun_elevation();
    const float t = std::clamp((elevation + 0.2f) / 0.5f, 0.0f, 1.0f);
    const float smooth_t = t * t * (3.0f - 2.0f * t);
    // Ночью не абсолютный чёрный (0.0), а тусклый минимум — см. комментарий в заголовке.
    constexpr float night_ambient = 0.4f;
    constexpr float day_ambient = 1.0f;
    return night_ambient + (day_ambient - night_ambient) * smooth_t;
}
