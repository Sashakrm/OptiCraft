#ifndef OPTICRAFT_WEATHER_CYCLE_H
#define OPTICRAFT_WEATHER_CYCLE_H

#include <glm/glm.hpp>
#include <random>

enum class Weather { Clear, Cloudy, Rain };

// Погода: ясно, облачно, дождь. Меняется сама через случайные интервалы (Config::weather_*).
// Сейчас влияет на цвет неба и общую яркость сцены; сама по себе не сохраняется в мире.
class Weather_Cycle {
public:
    Weather_Cycle();

    void update(float delta_time);

    Weather get_weather() const { return m_weather; }
    void set_weather(Weather weather); // для отладки/команд; сбрасывает таймер до следующей смены

    // Цвет неба с учётом погоды: облака — светлее и серее, дождь — тёмное серое небо.
    glm::vec3 apply_to_sky(const glm::vec3& sky_color) const;
    // Множитель на ambient (освещение поверхностей): облачно чуть темнее, в дождь заметно темнее.
    float ambient_multiplier() const;

private:
    Weather pick_next();

    Weather m_weather = Weather::Clear;
    float m_time_left = 0.0f;
    std::mt19937 m_rng;
};

#endif // OPTICRAFT_WEATHER_CYCLE_H
