#include "entities/effects/Status_Effects.h"

#include <algorithm>

#include "utils/Config.h"

std::optional<Potion_Info> potion_of(Block_Types type) {
    switch (type) {
        case Block_Types::Potion_Regeneration:
            return Potion_Info{Status_Effect::Regeneration, Config::potion_regen_duration, 0};
        case Block_Types::Potion_Speed:
            return Potion_Info{Status_Effect::Speed, Config::potion_speed_duration, 0};
        case Block_Types::Potion_Jump:
            return Potion_Info{Status_Effect::Jump_Boost, Config::potion_jump_duration, 0};
        default:
            return std::nullopt;
    }
}

bool is_potion(Block_Types type) {
    return potion_of(type).has_value();
}

void Status_Effects::apply(Status_Effect effect, float duration, int amplifier) {
    if (duration <= 0.0f) return;
    for (Effect_Slot& slot : m_slots) {
        if (slot.effect != effect) continue;
        slot.remaining = std::max(slot.remaining, duration);
        slot.amplifier = std::max(slot.amplifier, amplifier);
        return;
    }
    m_slots.push_back(Effect_Slot{effect, duration, amplifier});
}

void Status_Effects::clear() {
    m_slots.clear();
    m_regen_progress = 0.0f;
}

int Status_Effects::update(float dt) {
    int ticks = 0;
    if (const int level = amplifier(Status_Effect::Regeneration); level >= 0) {
        const float interval = Config::potion_regen_interval / static_cast<float>(level + 1);
        m_regen_progress += dt;
        while (m_regen_progress >= interval) {
            m_regen_progress -= interval;
            ++ticks;
        }
    } else {
        m_regen_progress = 0.0f;
    }

    for (Effect_Slot& slot : m_slots) slot.remaining -= dt;
    m_slots.erase(std::remove_if(m_slots.begin(), m_slots.end(),
        [](const Effect_Slot& slot) { return slot.remaining <= 0.0f; }), m_slots.end());
    return ticks;
}

bool Status_Effects::has(Status_Effect effect) const {
    return amplifier(effect) >= 0;
}

int Status_Effects::amplifier(Status_Effect effect) const {
    for (const Effect_Slot& slot : m_slots) {
        if (slot.effect == effect) return slot.amplifier;
    }
    return -1;
}

float Status_Effects::speed_factor() const {
    const int level = amplifier(Status_Effect::Speed);
    return level < 0 ? 1.0f : 1.0f + Config::potion_speed_bonus * static_cast<float>(level + 1);
}

float Status_Effects::jump_factor() const {
    const int level = amplifier(Status_Effect::Jump_Boost);
    return level < 0 ? 1.0f : 1.0f + Config::potion_jump_bonus * static_cast<float>(level + 1);
}
