#ifndef OPTICRAFT_BLOCK_INTERACTION_H
#define OPTICRAFT_BLOCK_INTERACTION_H

#include <glm/glm.hpp>
#include <optional>
#include <vector>
#include "physics/Raycast.h"
#include "world/blocks/Block_Types.h"
#include "entities/player/Inventory.h" // Item_Stack

class Chunk_Manager;
class Input_Manager;
class Renderer;
class Player;
class Crop_Manager;

// Сбросить кэш мешей чанка с блоком и соседних чанков на границе (после ручной правки блока).
void invalidate_block_mesh(int wx, int wy, int wz, Chunk_Manager& chunk_manager, Renderer& renderer);

// Сломанный в Survival блок: Game превращает это в выпавшие предметы (Dropped_Item_Manager).
// Содержимое контейнера снимается ДО удаления блока — потом Block_Entity уже стёрт.
struct Broken_Block_Event {
    glm::ivec3 position{0};
    Block_Types type = Block_Types::Air;
    std::vector<Item_Stack> container_contents;
    // false — инструмент не подходит для этого блока (руда без нужной кирки): дропа нет.
    bool harvested = true;
};

class Block_Interaction {
public:
    Block_Interaction();

    // entity_hit_distance — расстояние до ближайшего моба на луче взгляда (<0 — моба нет).
    // Если моб ближе блока, он «перехватывает» взгляд: блок не ломается, не подсвечивается
    // и ничего на него не ставится (атакой занимается Game).
    void update(float delta_time,
                const glm::vec3& eye_position,
                const glm::vec3& look_direction,
                const Input_Manager& input,
                Player& player,
                Chunk_Manager& chunk_manager,
                Renderer& renderer,
                Inventory& inventory,
                float entity_hit_distance = -1.0f);

    const Raycast_Hit& get_current_hit() const { return m_current_hit; }
    bool has_target() const { return m_current_hit.hit; }
    bool is_entity_targeted() const { return m_entity_targeted; }
    void set_place_block(Block_Types type) { m_place_block = type; }

    // Время добычи блока голыми руками в Survival, секунды (0 — мгновенно). Берётся из поля
    // break_time блока в blocks.json, а если его нет — hardness * Config::survival_break_seconds_per_hardness.
    // held — предмет в руке: подходящий инструмент ускоряет добычу в tool_speed раз.
    static float get_survival_break_time(Block_Types block, const Item_Stack& held = Item_Stack{});
    // Даст ли блок дроп, если ломать его этим предметом (руды требуют кирку нужного уровня).
    static bool can_harvest(Block_Types block, const Item_Stack& held);

    // Менеджер посевов (посадка по ПКМ семенами/морковью на грядку). nullptr — посадка выключена.
    void set_crop_manager(Crop_Manager* crops) { m_crop_manager = crops; }

    // Прогресс добычи текущего блока 0..1 (0 — не копаем) — для полоски под прицелом.
    float get_break_progress() const { return m_break_progress; }
    // Прогресс поедания 0..1 (0 — не едим).
    float get_eat_progress() const { return m_eat_progress; }

    // ПКМ по блоку-контейнеру (сундук/верстак/печь — см. Block_Entity.h) не ставит
    // блок на его грань, а запрашивает открытие экрана контейнера — см. update().
    // Забирается вызывающим кодом (Game) не чаще раза за клик: consume() очищает
    // запрос, поэтому повторный кадр без нового клика уже ничего не вернёт.
    bool has_container_open_request() const { return m_container_open_request.has_value(); }
    glm::ivec3 consume_container_open_request() {
        const glm::ivec3 pos = *m_container_open_request;
        m_container_open_request.reset();
        return pos;
    }

    // Блоки, сломанные с прошлого вызова (только Survival) — забирает Game каждый кадр.
    std::vector<Broken_Block_Event> take_broken_blocks() {
        std::vector<Broken_Block_Event> events;
        events.swap(m_broken_blocks);
        return events;
    }

    // Сбросить накопленное (смена режима, пауза, открытие экрана): иначе полоска добычи/еды
    // «зависла» бы на прежнем значении.
    void cancel_actions();

private:
    Raycast_Hit m_current_hit;
    float m_action_cooldown = 0.0f;
    static constexpr float k_action_delay = 0.15f;
    // После поломки блока в Survival — пауза до начала добычи следующего (как в Minecraft, 5 тиков).
    static constexpr float k_break_cooldown = 0.25f;
    static constexpr float k_max_reach = 6.0f;

    Block_Types m_place_block = Block_Types::Stone;
    Crop_Manager* m_crop_manager = nullptr;
    std::optional<glm::ivec3> m_container_open_request;

    // Добыча в Survival
    bool m_is_mining = false;
    glm::ivec3 m_mining_pos{0};
    Block_Types m_mining_type = Block_Types::Air;
    float m_mining_time = 0.0f;
    float m_break_progress = 0.0f;
    bool m_entity_targeted = false;

    // Еда
    float m_eat_timer = 0.0f;
    float m_eat_progress = 0.0f;
    Block_Types m_eating_type = Block_Types::Air;

    std::vector<Broken_Block_Event> m_broken_blocks;
};

#endif
