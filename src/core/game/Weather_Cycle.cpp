#include "core/game/Weather_Cycle.h"
#include "utils/Config.h"

#include <algorithm>
#include <random>

namespace {
    const glm::vec3 kCloudSkyTint(0.62f, 0.66f, 0.72f);
    const glm::vec3 kRainSkyTint(0.30f, 0.33f, 0.38f);
    constexpr float kCloudSkyMix = 0.45f;
    constexpr float kRainSkyMix = 0.75f;
    constexpr float kCloudAmbient = 0.85f;
    constexpr float kRainAmbient = 0.70f;
}

Weather_Cycle::Weather_Cycle()
    : m_rng(std::random_device{}())
{
    std::uniform_real_distribution<float> duration(Config::weather_min_duration, Config::weather_max_duration);
    m_time_left = duration(m_rng);
}

void Weather_Cycle::update(float delta_time) {
    m_time_left -= delta_time;
    if (m_time_left > 0.0f) return;

    m_weather = pick_next();
    std::uniform_real_distribution<float> duration(Config::weather_min_duration, Config::weather_max_duration);
    m_time_left = duration(m_rng);
}

void Weather_Cycle::set_weather(Weather weather) {
    m_weather = weather;
    std::uniform_real_distribution<float> duration(Config::weather_min_duration, Config::weather_max_duration);
    m_time_left = duration(m_rng);
}

Weather Weather_Cycle::pick_next() {
    // Веса переходов: из ясной погоды чаще тучи, из дождя чаще тучи или ясно.
    float clear_w = 0.0f, cloudy_w = 0.0f, rain_w = 0.0f;
    switch (m_weather) {
        case Weather::Clear:  clear_w = 0.50f; cloudy_w = 0.35f; rain_w = 0.15f; break;
        case Weather::Cloudy: clear_w = 0.35f; cloudy_w = 0.30f; rain_w = 0.35f; break;
        case Weather::Rain:   clear_w = 0.30f; cloudy_w = 0.45f; rain_w = 0.25f; break;
    }
    std::discrete_distribution<int> pick({clear_w, cloudy_w, rain_w});
    switch (pick(m_rng)) {
        case 0: return Weather::Clear;
        case 1: return Weather::Cloudy;
        default: return Weather::Rain;
    }
}

glm::vec3 Weather_Cycle::apply_to_sky(const glm::vec3& sky_color) const {
    switch (m_weather) {
        case Weather::Clear:  return sky_color;
        case Weather::Cloudy: return glm::mix(sky_color, sky_color * kCloudSkyTint, kCloudSkyMix);
        case Weather::Rain:   return glm::mix(sky_color, kRainSkyTint, kRainSkyMix);
    }
    return sky_color;
}

float Weather_Cycle::ambient_multiplier() const {
    switch (m_weather) {
        case Weather::Clear:  return 1.0f;
        case Weather::Cloudy: return kCloudAmbient;
        case Weather::Rain:   return kRainAmbient;
    }
    return 1.0f;
}
