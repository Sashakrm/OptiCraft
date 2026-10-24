#include "entities/animals/ai/Pig_AI.h"
#include "utils/logging/Logger.h"
#include <algorithm>
#include <random>

Pig_AI::Pig_AI(std::string name)
    : m_name(std::move(name))
    , m_hunger(30)
    , m_energy(80)
    , m_happiness(70)
    , m_rng(std::random_device{}()) {
}

int Pig_AI::roll_percent() {
    std::uniform_int_distribution<int> dist(0, 99);
    return dist(m_rng);
}

void Pig_AI::set_state(Pig_State new_state) {
    if (m_state == new_state) return;
    m_state = new_state;

    std::string message = "\U0001F437 " + m_name + " -> ";
    switch (m_state) {
        case Pig_State::Idle:     message += "стоит и думает о жизни"; break;
        case Pig_State::Hungry:   message += "проголодалась! Хрю-хрю!"; break;
        case Pig_State::Eating:   message += "ест из корыта (чавк-чавк)"; break;
        case Pig_State::Sleepy:   message += "зевает, хочет спать"; break;
        case Pig_State::Sleeping: message += "спит и видит сны о жёлудях"; break;
        case Pig_State::Walking:  message += "гуляет по двору"; break;
        case Pig_State::Happy:    message += "радостно хрюкает и виляет хвостиком!"; break;
    }
    //LOG_INFO(message); временно закоменчено
}

std::string Pig_AI::get_state_name() const {
    switch (m_state) {
        case Pig_State::Idle:     return "IDLE";
        case Pig_State::Hungry:   return "HUNGRY";
        case Pig_State::Eating:   return "EATING";
        case Pig_State::Sleepy:   return "SLEEPY";
        case Pig_State::Sleeping: return "SLEEPING";
        case Pig_State::Walking:  return "WALKING";
        case Pig_State::Happy:    return "HAPPY";
    }
    return "?";
}

void Pig_AI::update(float delta_time) {
    m_tick_accumulator += delta_time;
    while (m_tick_accumulator >= TICK_INTERVAL_SECONDS) {
        m_tick_accumulator -= TICK_INTERVAL_SECONDS;
        tick();
    }
}

void Pig_AI::tick() {
    // 1) Изменяем параметры со временем
    m_hunger    = std::min(100, m_hunger + 4);
    m_energy    = std::max(0,   m_energy - 2);
    m_happiness = std::max(0,   m_happiness - 1);

    // 2) Приоритеты поведения (чем выше, тем важнее): Голод > Сон > Скука
    if (m_hunger >= 80) {
        if (m_state != Pig_State::Eating) set_state(Pig_State::Eating);
    } else if (m_energy <= 15) {
        if (m_state != Pig_State::Sleeping) set_state(Pig_State::Sleeping);
    } else if (m_hunger >= 50 && m_state == Pig_State::Idle) {
        set_state(Pig_State::Hungry);
    } else if (m_energy <= 35 && m_state == Pig_State::Idle) {
        set_state(Pig_State::Sleepy);
    } else if (m_state == Pig_State::Idle) {
        set_state(roll_percent() % 2 == 0 ? Pig_State::Walking : Pig_State::Happy);
    }

    // 3) Поведение внутри текущего состояния
    switch (m_state) {
        case Pig_State::Eating:
            m_hunger -= 12;
            m_happiness += 3;
            if (m_hunger <= 10) {
                LOG_INFO("   \U0001F49A " + m_name + " наелась!");
                set_state(Pig_State::Happy);
            }
            break;

        case Pig_State::Sleeping:
            m_energy += 15;
            if (m_energy >= 90) {
                LOG_INFO("   \u2600\uFE0F " + m_name + " проснулась!");
                set_state(Pig_State::Idle);
            }
            break;

        case Pig_State::Hungry:
            if (m_hunger >= 80) set_state(Pig_State::Eating);
            break;

        case Pig_State::Sleepy:
            if (m_energy <= 15) set_state(Pig_State::Sleeping);
            break;

        case Pig_State::Walking:
            m_energy -= 3;
            m_happiness += 2;
            if (roll_percent() < 30) set_state(Pig_State::Idle);
            break;

        case Pig_State::Happy:
            m_happiness += 2;
            if (roll_percent() < 40) set_state(Pig_State::Idle);
            break;

        default: break;
    }

    // Ограничения
    m_hunger    = std::clamp(m_hunger, 0, 100);
    m_energy    = std::clamp(m_energy, 0, 100);
    m_happiness = std::clamp(m_happiness, 0, 100);
}
