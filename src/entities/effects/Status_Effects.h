//
// Status_Effects — временные эффекты, которые можно наложить на игрока и на живых существ
// (животных, мобов, лошадей). Эффекты накладываются зельями-шариками: шарик бросается ПКМ и
// разбивается о блок или моба, а все существа рядом получают эффект из него.
//
// Сейчас три эффекта для теста: регенерация (лечит), скорость (ходьба и бег) и сила прыжка
// (прыжок игрока и езда на лошади). Повторное наложение не складывает длительность: эффект
// продлевается до большего из двух сроков, а уровень (амплификатор) берётся максимальный.
//
// Класс ничего не знает про GL и мир: только таймеры и множители. Владелец сам применяет
// результат — например, update() возвращает число тиков регенерации, которые он добавит к HP.
//
#ifndef OPTICRAFT_STATUS_EFFECTS_H
#define OPTICRAFT_STATUS_EFFECTS_H

#include <cstdint>
#include <optional>
#include <vector>
#include "world/blocks/Block_Types.h"

enum class Status_Effect : uint8_t { Regeneration, Speed, Jump_Boost };

// Что даёт зелье-шарик: эффект, длительность в секундах и уровень (0 — первый).
struct Potion_Info {
    Status_Effect effect;
    float duration;
    int amplifier;
};

// Зелье по типу предмета; nullopt — предмет не зелье.
std::optional<Potion_Info> potion_of(Block_Types type);
bool is_potion(Block_Types type);

struct Effect_Slot {
    Status_Effect effect;
    float remaining;   // секунд до конца
    int amplifier;     // 0 — первый уровень, 1 — второй и т.д.
};

class Status_Effects {
public:
    // Накладывает эффект. Если он уже есть — срок продлевается до большего, уровень не падает.
    void apply(Status_Effect effect, float duration, int amplifier);
    void clear();

    // Уменьшает таймеры. Возвращает число тиков регенерации за этот шаг (1 тик = 1 HP).
    int update(float dt);

    bool has(Status_Effect effect) const;
    // Уровень эффекта, или -1, если эффекта нет.
    int amplifier(Status_Effect effect) const;
    bool empty() const { return m_slots.empty(); }
    const std::vector<Effect_Slot>& slots() const { return m_slots; }

    // Множители для движения. Без эффекта — 1.
    float speed_factor() const;
    float jump_factor() const;

private:
    std::vector<Effect_Slot> m_slots;
    float m_regen_progress = 0.0f;   // секунд, накопленных до следующего тика регенерации
};

#endif //OPTICRAFT_STATUS_EFFECTS_H
