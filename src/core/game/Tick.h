//
// Created by noktemor on 14.10.2026.
//

#ifndef OPTICRAFT_TICK_H
#define OPTICRAFT_TICK_H

//
// Tick.h — централизованные игровые тики (фиксированный шаг, 20 в секунду).
//
//  * Tick_Clock       — аккумулятор: превращает переменный dt кадра в целое число тиков.
//  * Tick_Scheduler   — реестр систем. Каждая система регистрируется с фазой (Pre / Simulate /
//                       Post) и порядком внутри фазы. Порядок не зависит от FPS.
//  * Tick_Event_Bus   — типизированные события. emit() ставит событие в очередь, доставка
//                       происходит в конце тика (flush), поэтому обработчик не меняет мир
//                       посреди работы другой системы.
//
// Системы получают фиксированный dt = Tick::SECONDS и ничего не знают о FPS.
// Ввод, камера и рендер остаются по кадрам и в тики не входят.
//

#include <cstdint>
#include <functional>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Tick {
    constexpr float SECONDS = 1.0f / 20.0f;  // длительность одного тика: 20 TPS
    constexpr int MAX_PER_FRAME = 10;         // предел тиков за кадр (защита от спирали смерти)
}

// --- События ----------------------------------------------------------------------------
// Типизированная шина: подписка и эмиссия работают с конкретным типом события, а внутри
// хранение стирается. Ошибка в типе данных ловится компилятором.
class Tick_Event_Bus {
public:
    template <typename E>
    void subscribe(std::function<void(const E&)> handler) {
        m_handlers[std::type_index(typeid(E))].push_back(
            [handler](const void* p) { handler(*static_cast<const E*>(p)); });
    }

    template <typename E>
    void emit(E event) {
        m_queue.push_back([this, e = std::move(event)]() mutable { dispatch(e); });
    }

    // Доставляет накопленные события. События, которые эмитят обработчики, уходят следующим
    // проходом; max_depth обрывает цикл, если обработчики эмитят друг друга бесконечно.
    void flush(int max_depth = 8) {
        for (int depth = 0; depth < max_depth && !m_queue.empty(); ++depth) {
            auto batch = std::move(m_queue);
            m_queue.clear();
            for (auto& deliver : batch) deliver();
        }
        m_queue.clear();
    }

    void clear() { m_queue.clear(); }
    std::size_t pending() const { return m_queue.size(); }

private:
    template <typename E>
    void dispatch(const E& event) {
        auto it = m_handlers.find(std::type_index(typeid(E)));
        if (it == m_handlers.end()) return;
        for (auto& handler : it->second) handler(&event);
    }

    std::unordered_map<std::type_index, std::vector<std::function<void(const void*)>>> m_handlers;
    std::vector<std::function<void()>> m_queue;
};

// --- Часы тиков -------------------------------------------------------------------------
class Tick_Clock {
public:
    // Возвращает, сколько тиков выполнить в этом кадре (0..MAX_PER_FRAME).
    int advance(float frame_dt, float speed = 1.0f);
    // Доля незавершённого тика [0, 1) — для будущей интерполяции визуала между тиками.
    float alpha() const;
    void reset();

private:
    float m_accumulator = 0.0f;
};

// --- Планировщик ------------------------------------------------------------------------
enum class Tick_Phase {
    Pre = 0,       // время, голод, таймеры
    Simulate = 1,  // мир, мобы, жидкости, культуры, предметы
    Post = 2       // уборка: удаление умерших существ и т.п.
};

class Tick_Scheduler {
public:
    using System_Fn = std::function<void(float dt)>;

    // Регистрация системы. Выполнение: сначала по фазе, затем по order (меньше — раньше).
    void add(const std::string& name, Tick_Phase phase, int order, System_Fn fn);

    // Вызывается каждый кадр, когда мир симулируется. Выполняет накопленные тики.
    // Возвращает число выполненных тиков.
    int run_frame(float frame_dt);
    void set_speed(float speed) { m_speed = speed; }
    float speed() const { return m_speed; }

    // Сброс аккумулятора и очереди событий (например, при смене мира).
    void reset();

    Tick_Event_Bus& events() { return m_events; }
    float alpha() const { return m_clock.alpha(); }
    std::uint64_t total_ticks() const { return m_total_ticks; }
    std::size_t system_count() const { return m_systems.size(); }

private:
    struct Entry {
        std::string name;
        Tick_Phase phase;
        int order;
        System_Fn fn;
    };

    void sort_if_needed();
    void run_one_tick();

    Tick_Clock m_clock;
    float m_speed = 1.0f;
    std::vector<Entry> m_systems;
    bool m_dirty = false;
    Tick_Event_Bus m_events;
    std::uint64_t m_total_ticks = 0;
};

#endif //OPTICRAFT_TICK_H
