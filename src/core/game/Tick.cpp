//
// Created by noktemor on 14.10.2026.
//

//
// Tick.cpp — реализация часов тиков и планировщика. Описание — в Tick.h.
//
#include "core/game/Tick.h"

#include <algorithm>

// --- Tick_Clock -------------------------------------------------------------------------

int Tick_Clock::advance(float frame_dt, float speed) {
    m_accumulator += frame_dt * speed;

    int ticks = 0;
    while (m_accumulator >= Tick::SECONDS && ticks < Tick::MAX_PER_FRAME) {
        m_accumulator -= Tick::SECONDS;
        ++ticks;
    }
    // Упёрлись в предел: отбрасываем остаток, иначе после лага игра будет догонять вечно.
    if (ticks == Tick::MAX_PER_FRAME) m_accumulator = 0.0f;
    return ticks;
}

float Tick_Clock::alpha() const {
    return m_accumulator / Tick::SECONDS;
}

void Tick_Clock::reset() {
    m_accumulator = 0.0f;
}

// --- Tick_Scheduler ---------------------------------------------------------------------

void Tick_Scheduler::add(const std::string& name, Tick_Phase phase, int order, System_Fn fn) {
    m_systems.push_back(Entry{name, phase, order, std::move(fn)});
    m_dirty = true;
}

void Tick_Scheduler::sort_if_needed() {
    if (!m_dirty) return;
    std::stable_sort(m_systems.begin(), m_systems.end(), [](const Entry& a, const Entry& b) {
        if (a.phase != b.phase) return a.phase < b.phase;
        return a.order < b.order;
    });
    m_dirty = false;
}

int Tick_Scheduler::run_frame(float frame_dt) {
    sort_if_needed();
    const int ticks = m_clock.advance(frame_dt, m_speed);
    for (int i = 0; i < ticks; ++i) {
        run_one_tick();
    }
    return ticks;
}

void Tick_Scheduler::run_one_tick() {
    for (auto& system : m_systems) {
        system.fn(Tick::SECONDS);
    }
    m_events.flush();
    ++m_total_ticks;
}

void Tick_Scheduler::reset() {
    m_clock.reset();
    m_events.clear();
}
