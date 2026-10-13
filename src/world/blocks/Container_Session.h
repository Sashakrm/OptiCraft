//
// Container_Session — открытая на экране "начинка" одного блока-контейнера (сундук/
// печь/верстак): курсор-стак + логика клика, которая двигает предметы между
// Block_Entity::slots и Inventory (хотбар игрока), по той же модели "курсор
// подхватывает/кладёт/сливает стак", что и Inventory::click_panel_slot — которую
// эта сессия для хотбарной части и переиспользует напрямую (Inventory::set_carried_stack
// + click_panel_slot), а не дублирует своей копией.
//
// Расклад слотов для click_slot(index):
//   [0, container_slot_count)                — слоты самого контейнера
//   [container_slot_count, +hotbar_size)      — хотбар игрока
//   для верстака отдельно: container_slot_count == 9 (сетка), а результат крафта —
//   ВИРТУАЛЬНЫЙ слот сразу после сетки (см. get_output_slot_index), в Block_Entity
//   не хранится и считается на лету через Recipe_Registry.
//
#ifndef OPTICRAFT_CONTAINER_SESSION_H
#define OPTICRAFT_CONTAINER_SESSION_H

#include <algorithm>
#include <array>
#include <utility>
#include "world/blocks/Block_Entity.h"
#include "world/crafting/Recipe.h"
#include "world/crafting/Smelting.h"
#include "entities/player/Inventory.h"

class Container_Session {
public:
    void open(Block_Entity& entity) {
        m_entity = &entity;
    }

    // Стак на курсоре не должен потеряться, если экран закрыли, пока в руке что-то
    // было — в Survival раскладываем обратно по инвентарю (как Inventory::drop_carried_
    // into_inventory), в Creative просто отпускаем (там это лишняя копия).
    void close(Inventory& inventory) {
        if (!m_carried.is_empty()) {
            if (inventory.get_mode() == Game_Mode::Survival) {
                inventory.add_resource(m_carried.type, m_carried.count);
            }
            m_carried = Item_Stack{};
        }
        m_entity = nullptr;
    }

    bool is_open() const { return m_entity != nullptr; }
    const Block_Entity* get_entity() const { return m_entity; }
    Block_Entity_Type get_type() const { return m_entity ? m_entity->type : Block_Entity_Type::None; }
    size_t get_container_slot_count() const { return m_entity ? m_entity->slots.size() : 0; }

    // Верстак: виртуальный слот результата крафта (k_output_slot) — не входит
    // в get_container_slot_count(), рисуется/кликается отдельно вызывающим кодом (Game).
    bool has_output_slot() const { return m_entity && m_entity->type == Block_Entity_Type::Crafting_Table; }
    // Особый индекс-маркер, а не "следующий после сетки": иначе он совпал бы с первым
    // слотом хотбара в раскладке click_slot (там хотбар идёт сразу за слотами контейнера).
    static constexpr size_t k_output_slot = static_cast<size_t>(-1);
    size_t get_output_slot_index() const { return k_output_slot; }

    Item_Stack get_container_slot(size_t index) const {
        if (!m_entity || index >= m_entity->slots.size()) return {};
        return m_entity->slots[index];
    }

    // Только для верстака: что получится, если скрафтить прямо сейчас (для превью
    // в HUD, до фактического клика) — пусто, если рецепт не подобрался.
    Item_Stack peek_crafting_output() const {
        if (!has_output_slot()) return {};
        const Recipe* recipe = find_matching_recipe();
        if (!recipe) return {};
        return Item_Stack{ recipe->output_type, recipe->output_count };
    }

    Item_Stack get_carried() const { return m_carried; }
    bool has_carried() const { return !m_carried.is_empty(); }

    // index — как описано в комментарии класса. paired — вторая половина двойного
    // сундука (см. Chunk_Manager::get_paired_block_entity), может быть nullptr.
    void click_slot(size_t index, Inventory& inventory, Block_Entity* paired = nullptr, bool right = false) {
        if (!m_entity) return;

        const size_t own_count = m_entity->slots.size();
        const size_t paired_count = paired ? paired->slots.size() : 0;

        if (has_output_slot() && index == get_output_slot_index()) {
            click_crafting_output();
            return;
        }

        if (index < own_count) {
            if (m_entity->type == Block_Entity_Type::Furnace) {
                // Выход печи — только забрать: класть туда руками нельзя (как в Minecraft).
                if (index == Block_Entity_Slots::furnace_output && !m_carried.is_empty()) {
                    return;
                }
                // В слот топлива кладётся только то, что горит (см. assets/smelting.json).
                if (index == Block_Entity_Slots::furnace_fuel && !m_carried.is_empty() &&
                    Smelting_Registry::get_instance().get_fuel_time(m_carried.type) <= 0.0f) {
                    return;
                }
            }
            click_container_slot(m_entity->slots[index], right);
            return;
        }
        index -= own_count;

        if (paired && index < paired_count) {
            click_container_slot(paired->slots[index], right);
            return;
        }
        if (paired) index -= paired_count;

        // Дальше — слоты игрока: полная панель 0..35 (27 хранилище + 9 хотбар),
        // тот же индекс, что у Inventory::click_panel_slot.
        if (index >= Inventory::k_panel_slot_count) return;

        if (inventory.get_mode() == Game_Mode::Creative) {
            // В Creative показывается только хотбар (см. Game::build_container_panel).
            if (index < Inventory::k_panel_hotbar_start) return;
            index -= Inventory::k_panel_hotbar_start;
            // Бесконечный запас: пустая рука берёт 1 шт. текущего блока слота, занятая
            // просто освобождается — относить блок обратно в Creative бессмысленно
            // (симметрично тому, как уже ведёт себя хотбарная строка в самой E-панели).
            const Item_Stack hotbar_slot = inventory.get_hotbar_slot(index);
            m_carried = (m_carried.is_empty() && !hotbar_slot.is_empty())
                       ? Item_Stack{hotbar_slot.type, 1}
                       : Item_Stack{};
            return;
        }

        inventory.set_carried_stack(m_carried);
        if (right) inventory.right_click_panel_slot(index);
        else inventory.click_panel_slot(index);
        m_carried = inventory.get_carried_stack();
        inventory.set_carried_stack(Item_Stack{});
    }

    // Shift+ЛКМ (только Survival): из контейнера — в сумку игрока, из сумки — в контейнер.
    // index — как в click_slot.
    void quick_move(size_t index, Inventory& inventory, Block_Entity* paired = nullptr) {
        if (!m_entity || inventory.get_mode() != Game_Mode::Survival) return;

        if (has_output_slot() && index == get_output_slot_index()) {
            for (int guard = 0; guard < 64; ++guard) {
                const Recipe* recipe = find_matching_recipe();
                if (!recipe) return;
                const int added = inventory.try_add(recipe->output_type, recipe->output_count);
                if (added < recipe->output_count) {
                    if (added > 0) inventory.remove_resource(recipe->output_type, added);
                    return;
                }
                for (Item_Stack& cell : m_entity->slots) {
                    if (cell.is_empty()) continue;
                    if (--cell.count <= 0) cell = Item_Stack{};
                }
            }
            return;
        }

        const size_t own_count = m_entity->slots.size();
        const size_t paired_count = paired ? paired->slots.size() : 0;

        if (index < own_count + paired_count) {
            Item_Stack& slot = index < own_count ? m_entity->slots[index] : paired->slots[index - own_count];
            if (slot.is_empty()) return;
            const int added = inventory.try_add_stack(slot);
            slot.count -= added;
            if (slot.count <= 0) slot = Item_Stack{};
            return;
        }

        Item_Stack* source = inventory.get_panel_slot_mutable(index - own_count - paired_count);
        if (!source || source->is_empty()) return;

        // Куда можно класть: печь — топливо или вход; остальные — любые слоты.
        std::vector<Item_Stack*> targets;
        if (m_entity->type == Block_Entity_Type::Furnace) {
            const bool fuel = Smelting_Registry::get_instance().get_fuel_time(source->type) > 0.0f;
            targets.push_back(&m_entity->slots[fuel ? Block_Entity_Slots::furnace_fuel : Block_Entity_Slots::furnace_input]);
        } else {
            for (Item_Stack& s : m_entity->slots) targets.push_back(&s);
            if (paired) for (Item_Stack& s : paired->slots) targets.push_back(&s);
        }
        for (Item_Stack* t : targets) {           // сначала докладываем в такие же стаки
            if (source->is_empty()) break;
            if (!t->is_empty() && t->type == source->type && t->damage == source->damage &&
                t->count < max_stack_for(t->type)) {
                const int moved = std::min(max_stack_for(t->type) - t->count, source->count);
                t->count += moved;
                source->count -= moved;
            }
        }
        for (Item_Stack* t : targets) {           // потом в пустые
            if (source->count <= 0) break;
            if (t->is_empty()) {
                const int moved = std::min(max_stack_for(source->type), source->count);
                *t = *source;
                t->count = moved;
                source->count -= moved;
            }
        }
        if (source->count <= 0) *source = Item_Stack{};
    }

private:
    Block_Entity* m_entity = nullptr;
    Item_Stack m_carried;

    void click_container_slot(Item_Stack& slot, bool right = false) {
        if (right) {
            if (m_carried.is_empty()) {
                if (slot.is_empty()) return;
                const int take = (slot.count + 1) / 2;
                m_carried = slot;
                m_carried.count = take;
                slot.count -= take;
                if (slot.count <= 0) slot = Item_Stack{};
                return;
            }
            if (slot.is_empty()) {
                slot = m_carried;
                slot.count = 1;
            } else if (slot.type == m_carried.type && slot.damage == m_carried.damage &&
                       slot.count < max_stack_for(slot.type)) {
                slot.count += 1;
            } else {
                std::swap(slot, m_carried);
                return;
            }
            if (--m_carried.count <= 0) m_carried = Item_Stack{};
            return;
        }
        // Досыпаем в существующий стак того же типа вместо того, чтобы менять местами.
        if (!m_carried.is_empty() && !slot.is_empty() && slot.type == m_carried.type &&
            slot.damage == m_carried.damage && slot.count < max_stack_for(slot.type)) {
            const int space = max_stack_for(slot.type) - slot.count;
            const int moved = std::min(space, m_carried.count);
            slot.count += moved;
            m_carried.count -= moved;
            if (m_carried.count <= 0) m_carried = Item_Stack{};
            return;
        }
        std::swap(slot, m_carried);
    }

    const Recipe* find_matching_recipe() const {
        if (!m_entity || m_entity->slots.size() != 9) return nullptr;
        std::array<Item_Stack, 9> grid{};
        for (size_t i = 0; i < 9; ++i) grid[i] = m_entity->slots[i];
        return Recipe_Registry::get_instance().match(grid);
    }

    void click_crafting_output() {
        // Как в Minecraft: клик по слоту результата ничего туда не кладёт — только
        // забирает готовый предмет (и только если рука пуста или там уже лежит то же
        // самое и есть место), одновременно списывая по 1 штуке каждого ингредиента
        // из сетки. Само устройство рецептов — см. Recipe_Registry.
        const Recipe* recipe = find_matching_recipe();
        if (!recipe) return;

        const Item_Stack result{ recipe->output_type, recipe->output_count };
        if (!m_carried.is_empty() &&
            !(m_carried.type == result.type && m_carried.damage == 0 &&
              m_carried.count + result.count <= max_stack_for(result.type))) {
            return; // рука занята чем-то несовместимым — забрать некуда
        }

        for (Item_Stack& cell : m_entity->slots) {
            if (cell.is_empty()) continue;
            cell.count -= 1;
            if (cell.count <= 0) cell = Item_Stack{};
        }

        if (m_carried.is_empty()) {
            m_carried = result;
        } else {
            m_carried.count += result.count;
        }
    }
};

#endif //OPTICRAFT_CONTAINER_SESSION_H
