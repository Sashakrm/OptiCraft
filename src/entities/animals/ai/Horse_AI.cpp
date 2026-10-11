#include "entities/animals/ai/Horse_AI.h"
#include <algorithm>

Horse_AI::Horse_AI(std::string name)
    : m_name(std::move(name))
    , m_rng(std::random_device{}()) {
    // Лошади рождаются в разном настроении: иначе все заспавненные разом синхронно «думают».
    m_hunger = rand_range(60.0f, 100.0f);
    m_energy = rand_range(60.0f, 100.0f);
    m_next_decision = rand_range(0.5f, 4.0f);
}

float Horse_AI::rand01() {
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng);
}

float Horse_AI::rand_range(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(m_rng);
}

std::string Horse_AI::get_state_name() const {
    switch (m_state) {
        case Horse_State::Idle:   return "Idle";
        case Horse_State::Wander: return "Wander";
        case Horse_State::Graze:  return "Graze";
        case Horse_State::Happy:  return "Happy";
        case Horse_State::Scared: return "Scared";
        case Horse_State::Sleep:  return "Sleep";
        case Horse_State::Taming: return "Taming";
        case Horse_State::Ridden: return "Ridden";
        case Horse_State::Flee:   return "Flee";
    }
    return "?";
}

bool Horse_AI::is_urgent(Horse_State state) {
    return state == Horse_State::Flee || state == Horse_State::Scared ||
           state == Horse_State::Taming || state == Horse_State::Ridden;
}

void Horse_AI::enter_state(Horse_State state) {
    m_state = state;
    m_state_timer = 0.0f;

    switch (state) {
        case Horse_State::Idle:   m_next_decision = rand_range(2.0f, 5.0f);  break;
        case Horse_State::Wander: m_next_decision = rand_range(3.0f, 6.0f);  break;
        case Horse_State::Graze:  m_next_decision = rand_range(4.0f, 8.0f);  break;
        case Horse_State::Happy:  m_next_decision = rand_range(1.5f, 3.0f);  break;
        case Horse_State::Sleep:  m_next_decision = rand_range(8.0f, 15.0f); break;
        case Horse_State::Scared: m_next_decision = rand_range(1.5f, 3.0f);  break;
        case Horse_State::Flee:   m_next_decision = rand_range(3.0f, 6.0f);  break;
        default: break;
    }
}

void Horse_AI::add_fear(float amount, const glm::vec3& fear_source) {
    m_fear = std::clamp(m_fear + amount, 0.0f, 100.0f);
    m_fear_source = fear_source;
}

void Horse_AI::choose_next_idle_state() {
    // Голодная лошадь почти наверняка идёт есть.
    if (m_hunger < 40.0f) {
        enter_state(Horse_State::Graze);
        return;
    }

    const float r = rand01();
    if      (r < 0.35f) enter_state(Horse_State::Wander);
    else if (r < 0.55f) enter_state(Horse_State::Graze);
    else if (r < 0.75f) enter_state(Horse_State::Idle);
    else if (r < 0.90f) enter_state(Horse_State::Happy);
    else                enter_state(Horse_State::Sleep);
}

void Horse_AI::update_rider(float delta_time) {
    m_state_timer += delta_time;
    m_fear = std::max(0.0f, m_fear - 10.0f * delta_time); // рядом с всадником спокойнее
}

void Horse_AI::update_free(float delta_time) {
    m_state_timer += delta_time;
    m_next_decision -= delta_time;

    // Потребности тикают всегда.
    m_hunger = std::clamp(m_hunger - 0.5f * delta_time, 0.0f, 100.0f);
    m_energy = std::clamp(m_energy - 0.3f * delta_time, 0.0f, 100.0f);
    m_fear   = std::clamp(m_fear   - 2.0f * delta_time, 0.0f, 100.0f);

    // Экстренные переходы — приоритет выше всего.
    if (m_fear > 70.0f && m_state != Horse_State::Flee) {
        enter_state(Horse_State::Flee);
    }
    if (m_energy < 10.0f && m_state != Horse_State::Sleep &&
        m_state != Horse_State::Flee && m_state != Horse_State::Scared) {
        enter_state(Horse_State::Sleep);
    }

    // Состояния-реакции заканчиваются, когда страх угас.
    if (m_state == Horse_State::Scared && m_fear < 20.0f) {
        choose_next_idle_state();
    } else if (m_state == Horse_State::Flee && m_fear < 15.0f && m_next_decision <= 0.0f) {
        enter_state(Horse_State::Idle);
    }

    // Обычные переходы по таймеру.
    if (m_next_decision <= 0.0f && !is_urgent(m_state)) {
        // Поев, лошадь часто радуется (как в гайде).
        if (m_state == Horse_State::Graze && m_hunger > 80.0f) {
            enter_state(Horse_State::Happy);
        } else {
            choose_next_idle_state();
        }
    }

    // Эффекты состояний на потребности.
    if (m_state == Horse_State::Graze && m_state_timer > 2.0f) {
        m_hunger = std::min(m_hunger + 15.0f * delta_time, 100.0f);
    } else if (m_state == Horse_State::Sleep) {
        m_energy = std::min(m_energy + 8.0f * delta_time, 100.0f);
        if (m_energy > 95.0f) enter_state(Horse_State::Idle);
    }
}
