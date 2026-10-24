#ifndef OPTICRAFT_LOGIC_MANAGER_H
#define OPTICRAFT_LOGIC_MANAGER_H

// Красная логика движка (собственная реализация по правилам Java Edition).
//
// Время: редстоун-тик = 2 игровых тика = 0.1 с. Все элементы переключаются на границе тика.
// Сигнал: 0..15.
//
// Правила:
//  * Провод: сигнал на 1 меньше на каждый шаг по проводам (от 15, до 0). Получает сигнал от
//    включённого рычага или кнопки (15), от горящего зажимателя (15, кроме своей опоры), от
//    повторителя, который смотрит на него (15), и от твёрдого блока, на который подан сигнал.
//  * Рычаг: переключается игроком мгновенно. Включённый даёт 15 во все соседние клетки.
//  * Кнопка: нажатие даёт 15 на 10 редстоун-тиков (1 с), затем отпускается. Нажатие во время
//    сигнала ничего не меняет.
//  * Зажиматель сигнала (инвертор): горит, пока блок-опора (под ним или на грани, к которой
//    прикреплён) не получает сигнал. Гаснет/загорается с задержкой 1 редстоун-тик. Даёт 15 во
//    все стороны, кроме опоры.
//  * Повторитель: вход сзади (по направлению выхода — противоположная сторона), выход только
//    вперёд. Задержка 1–4 редстоун-тика (ПКМ циклически меняет). Если вход пропал до конца
//    задержки, переключение отменяется. Блокируется (держит состояние), если в него с боку
//    выдаёт сигнал другой включённый повторитель.
//
// Что не повторено из Java Edition (упрощения): квазисвязность, разделение сильного и слабого
// сигнала, порядок обновления соседей, сгорание зажимателя при частых переключениях.
// Сверено с описанием механик, а не с исходным кодом игры — сравнивайте поведение в игре.
//
// Состояние: тип блока (включён/выключен, нажата) и метаданные (грань крепления, направление
// и задержка повторителя) хранятся в чанках. Задержки и таймер кнопки живут в памяти и
// восстанавливаются при обнаружении узлов в мире.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>
#include <glm/glm.hpp>
#include "world/blocks/Block_Types.h"

using Redstone_Key = std::array<int, 3>;

class Logic_Manager {
public:
    using Block_Reader = std::function<Block_Types(int x, int y, int z)>;
    using Meta_Reader = std::function<uint8_t(int x, int y, int z)>;
    using Block_Writer = std::function<void(int x, int y, int z, Block_Types type, uint8_t meta)>;
    using Loaded_Check = std::function<bool(int x, int y, int z)>;

    // Доступ к миру. Передаётся извне: менеджер не знает про Chunk_Manager и проверяется без него.
    struct World_Access {
        Block_Reader type;
        Meta_Reader meta;
        Block_Writer write;
        Loaded_Check loaded;
    };

    static constexpr float k_redstone_tick_seconds = 0.1f;
    static constexpr int k_button_ticks = 10;       // 10 редстоун-тиков = 1 с, как каменная кнопка
    static constexpr int k_clamp_delay_ticks = 1;

    // --- Классификация блоков ---
    static bool is_wire(Block_Types type);
    // Любой элемент красной логики (провод, рычаг, кнопка, зажиматель, повторитель, и их состояния).
    static bool is_node(Block_Types type);
    // Крепятся к грани блока-опоры (рычаг, кнопка, зажиматель). Грань — в метаданных (0..5).
    static bool is_attached(Block_Types type);
    // Опора: твёрдый блок, который не является элементом логики.
    static bool is_support(Block_Types type);
    // Предмет, который выпадает при поломке: Lever_On → Lever, Repeater_On → Repeater, и т.д.
    static Block_Types item_of(Block_Types type);
    // Блок-опора узла (для провода и повторителя — блок снизу, для прикреплённых — по грани).
    static glm::ivec3 support_of(glm::ivec3 pos, Block_Types type, uint8_t meta);

    // --- Установка ---
    // Можно ли поставить тип в place, прикрепив к clicked (блок, на грань которого ставят).
    // repeater_facing — направление выхода повторителя (Block_Meta::facing_*). out_meta — метаданные.
    static bool placement(Block_Types type, const World_Access& world, glm::ivec3 place,
                          glm::ivec3 clicked, uint8_t repeater_facing, uint8_t& out_meta);

    // Есть ли у узла опора (для проверки после ломания).
    static bool has_support_for(const World_Access& world, glm::ivec3 pos, Block_Types type, uint8_t meta);

    // --- Ломание опоры ---
    // Узлы рядом с клеткой pos, которые остались без опоры после того, как pos сломали.
    // Их нужно сломать и выбросить предмет (item_of). Сам pos уже должен быть воздухом.
    static std::vector<glm::ivec3> nodes_without_support(const World_Access& world, glm::ivec3 pos);

    // --- Действия игрока ---
    // ПКМ по рычагу (переключает), кнопке (нажимает), повторителю (меняет задержку).
    // true — узел обработан, и ставить блок по ПКМ не нужно.
    bool interact(const World_Access& world, glm::ivec3 pos);

    // --- Симуляция ---
    // Запомнить узел для симуляции (ставится при установке и при обнаружении в мире).
    void track(glm::ivec3 pos);
    // Найти узлы в кубе вокруг center (радиус radius) и запомнить их. Дорого: вызывать раз в секунду.
    void discover(const World_Access& world, glm::ivec3 center, int radius);
    // Накопить время и выполнить нужное число редстоун-тиков.
    void update(const World_Access& world, float dt);
    // Один редстоун-тик (вызывается из update; открыто для тестов).
    void step(const World_Access& world);

    size_t tracked_count() const { return m_nodes.size(); }

    // Клетки, которые менеджер изменил с прошлого вызова (для обновления мешей). Забирает Game.
    std::vector<glm::ivec3> take_changed();

private:
    // Единственная точка записи в мир: запоминает изменённые клетки.
    void write_block(const World_Access& world, glm::ivec3 pos, Block_Types type, uint8_t meta);

    std::vector<glm::ivec3> m_changed;
    std::set<Redstone_Key> m_nodes;
    struct Pending {
        int remaining = 0;
        Block_Types target = Block_Types::Air;
    };
    std::map<Redstone_Key, Pending> m_pending;   // отложенные переключения зажимателя и повторителя
    std::map<Redstone_Key, int> m_pulses;         // сколько тиков кнопка ещё нажата
    float m_accumulator = 0.0f;
};

// Доступ к миру через Chunk_Manager (для игры). Тесты задают World_Access сами.
class Chunk_Manager;
Logic_Manager::World_Access make_chunk_world(Chunk_Manager& chunks);

#endif //OPTICRAFT_LOGIC_MANAGER_H
