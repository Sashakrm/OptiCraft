// Fluid_Simulator — растекание жидкостей по правилам Minecraft (Java Edition).
//
//  * Жидкость — обычный блок (state == Liquid в blocks.json), а её уровень лежит в метаданных
//    блока (см. Block_Meta::fluid_*): 0 = источник, 1..7 = течение (7 — самая тонкая струйка),
//    бит 0x08 = падающая вода. Море из генератора мира — источники с meta = 0, поэтому
//    метаданных не хранит.
//  * Симуляция по тикам (1 тик = Config::fluid_tick_seconds): клетка, рядом с которой что-то
//    изменилось, ставит себе отложенный тик через fluid_tick_delay (вода — 5 тиков). Тик
//    пересчитывает уровень клетки из соседей и растекает жидкость дальше. Ничего не
//    пересчитывается, пока не было изменений, поэтому спокойное море почти ничего не стоит.
//  * Правила (как в FlowingFluid): вода сначала течёт вниз; если снизу твёрдый блок —
//    растекается в стороны, предпочитая направление к ближайшему обрыву (поиск до
//    fluid_slope_distance блоков); каждый шаг в сторону теряет fluid_drop_off уровней; из двух
//    соседних источников над твёрдым блоком или источником получается новый источник;
//    без питания течение отступает.
//  * Чанки, которые не загружены, не симулируются, а жидкость считает их стеной.
#ifndef OPTICRAFT_FLUID_SIMULATOR_H
#define OPTICRAFT_FLUID_SIMULATOR_H

#include "math/Hash_Utils.h"
#include "world/blocks/Block_Types.h"

#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

class Chunk_Manager;

class Fluid_Simulator {
public:
    // Клетка (и её соседи) изменилась: назначить отложенные тики тем из них, где есть жидкость.
    void notify_changed(Chunk_Manager& chunks, const glm::ivec3& position);
    // Поставить тик конкретной клетке (при загрузке чанка с текущей водой и т.п.).
    void schedule(const glm::ivec3& position, int delay_ticks);

    // Продвинуть симуляцию на delta_time секунд (звать каждый кадр из главного потока).
    void update(Chunk_Manager& chunks, float delta_time);

    void clear();
    size_t pending_count() const { return m_scheduled.size(); }
    uint64_t current_tick() const { return m_tick; }

private:
    struct Entry {
        uint64_t due;
        uint64_t sequence;
        glm::ivec3 position;
        bool operator>(const Entry& other) const {
            return due != other.due ? due > other.due : sequence > other.sequence;
        }
    };

    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> m_queue;
    // Ближайший назначенный срок для клетки: запись в очереди с другим сроком считается устаревшей.
    std::unordered_map<glm::ivec3, uint64_t, Chunk_Key_Hash> m_scheduled;
    uint64_t m_tick = 0;
    uint64_t m_sequence = 0;
    float m_accumulator = 0.0f;
};

#endif // OPTICRAFT_FLUID_SIMULATOR_H
