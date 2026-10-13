//
// Created by noktemor on 26.08.2026.
//

#include "entities/player/Inventory.h"
#include "world/crafting/Recipe.h"
#include <algorithm>
#include <array>
#include <sstream>

Inventory::Inventory() {
    // Верхняя граница — вся ёмкость реестра (k_max_block_types), а не фиксированный
    // Block_Types::Count: блок, добавленный только в blocks.json (без нового имени в enum),
    // тоже появится в полном инвентаре без пересборки движка. Проверка на пустое имя отсекает
    // незанятые слоты, так что лишних итераций по факту не видно нигде, кроме старта игры.
    for (Block_ID id = 1; id < k_max_block_types; ++id) {
        if (Block_Registry::get_instance().is_reserved(id)) continue;
        if (get_block_props(id).name.empty()) continue; // пропуск незанятых/зарезервированных id
        if (!get_block_props(id).show_in_creative) continue;
        m_all_blocks.push_back(static_cast<Block_Types>(id));
    }

    // Creative-хотбар как в Minecraft creative: изначально заполняется первыми k_hotbar_size
    // блоками реестра, а дальше игрок сам решает, что в нём лежит (см. pick_grid_slot).
    const size_t initial_count = std::min(k_hotbar_size, m_all_blocks.size());
    m_creative_hotbar.assign(m_all_blocks.begin(), m_all_blocks.begin() + static_cast<long>(initial_count));
    m_creative_hotbar.resize(k_hotbar_size, Block_Types::Air);

    // Survival начинается пустым — и хотбар, и основная сетка наполняются по мере добычи.
    m_survival_hotbar.assign(k_hotbar_size, Item_Stack{});
    m_survival_storage.assign(k_survival_storage_size, Item_Stack{});
}

void Inventory::select_next() {
    m_selected_index = (m_selected_index + 1) % static_cast<int>(k_hotbar_size);
}

void Inventory::select_previous() {
    m_selected_index = (m_selected_index - 1 + static_cast<int>(k_hotbar_size)) % static_cast<int>(k_hotbar_size);
}

void Inventory::select_index(size_t index) {
    if (index >= k_hotbar_size) return;
    m_selected_index = static_cast<int>(index);
}

Item_Stack Inventory::get_hotbar_slot(size_t index) const {
    if (m_mode == Game_Mode::Creative) {
        if (index >= m_creative_hotbar.size()) return {};
        return { m_creative_hotbar[index], 0 }; // count=0 — HUD не рисует число (запас бесконечен)
    }
    if (index >= m_survival_hotbar.size()) return {};
    return m_survival_hotbar[index];
}

// --- Панель 9 x 4 ----------------------------------------------------------
Block_Types Inventory::creative_block_at(size_t panel_index) const {
    if (panel_index >= k_panel_storage_slots) return Block_Types::Air;
    const size_t absolute = m_creative_page * k_panel_storage_slots + panel_index;
    if (absolute >= m_all_blocks.size()) return Block_Types::Air;
    return m_all_blocks[absolute];
}

size_t Inventory::get_page_count() const {
    if (m_mode != Game_Mode::Creative) return 1;
    if (m_all_blocks.empty()) return 1;
    return (m_all_blocks.size() + k_panel_storage_slots - 1) / k_panel_storage_slots;
}

void Inventory::next_page() {
    const size_t pages = get_page_count();
    if (pages <= 1) return;
    m_creative_page = (m_creative_page + 1) % pages;
}

void Inventory::previous_page() {
    const size_t pages = get_page_count();
    if (pages <= 1) return;
    m_creative_page = (m_creative_page + pages - 1) % pages;
}

Item_Stack Inventory::get_panel_slot(size_t index) const {
    if (index >= k_panel_slot_count) return {};

    // Нижняя строка панели — это тот же хотбар, что виден на экране.
    if (index >= k_panel_hotbar_start) {
        return get_hotbar_slot(index - k_panel_hotbar_start);
    }

    if (m_mode == Game_Mode::Creative) {
        const Block_Types block = creative_block_at(index);
        // count = 0 — HUD не рисует число: запас в Creative бесконечен.
        return block == Block_Types::Air ? Item_Stack{} : Item_Stack{ block, 0 };
    }

    if (index >= m_survival_storage.size()) return {};
    return m_survival_storage[index];
}

void Inventory::click_panel_slot(size_t index) {
    if (index >= k_panel_slot_count) return;

    const bool is_hotbar_slot = index >= k_panel_hotbar_start;
    const size_t hotbar_index = is_hotbar_slot ? index - k_panel_hotbar_start : 0;

    if (m_mode == Game_Mode::Creative) {
        if (!is_hotbar_slot) {
            // Верхние строки Creative — бесконечный список блоков. Клик по ним
            // всегда берёт новую копию блока на курсор (или очищает курсор, если
            // кликнули по пустому слоту в конце последней страницы).
            const Block_Types block = creative_block_at(index);
            m_carried = (block == Block_Types::Air) ? Item_Stack{} : Item_Stack{ block, 1 };
            return;
        }

        // Клик по хотбару: рука с блоком кладёт его в слот, пустая — очищает слот.
        m_creative_hotbar[hotbar_index] = m_carried.is_empty()
                                        ? Block_Types::Air : m_carried.type;
        return;
    }

    // Survival: обычный обмен "слот <-> курсор". Одна и та же ветка обслуживает
    // и хранилище, и хотбар, поэтому блок можно положить в любой из 36 слотов.
    Item_Stack& slot = is_hotbar_slot ? m_survival_hotbar[hotbar_index]
                                      : m_survival_storage[index];

    left_click_stack(slot, m_carried);
}

void Inventory::left_click_stack(Item_Stack& slot, Item_Stack& carried) {
    // Клик по слоту с тем же блоком — досыпаем в стак, а не меняем местами.
    if (!carried.is_empty() && !slot.is_empty() && slot.type == carried.type &&
        slot.damage == carried.damage && slot.count < max_stack_for(slot.type)) {
        const int space = max_stack_for(slot.type) - slot.count;
        const int moved = std::min(space, carried.count);
        slot.count += moved;
        carried.count -= moved;
        if (carried.count <= 0) carried = Item_Stack{};
        return;
    }
    std::swap(slot, carried);
}

void Inventory::right_click_stack(Item_Stack& slot, Item_Stack& carried) {
    if (carried.is_empty()) {
        // Пустая рука: берём половину (с округлением вверх).
        if (slot.is_empty()) return;
        const int take = (slot.count + 1) / 2;
        carried = slot;
        carried.count = take;
        slot.count -= take;
        if (slot.count <= 0) slot = Item_Stack{};
        return;
    }
    // В руке стак: кладём ровно один предмет в пустой слот или в стак того же типа.
    if (slot.is_empty()) {
        slot = carried;
        slot.count = 1;
    } else if (slot.type == carried.type && slot.damage == carried.damage &&
               slot.count < max_stack_for(slot.type)) {
        slot.count += 1;
    } else {
        std::swap(slot, carried);
        return;
    }
    carried.count -= 1;
    if (carried.count <= 0) carried = Item_Stack{};
}

void Inventory::right_click_panel_slot(size_t index) {
    if (index >= k_panel_slot_count) return;
    if (m_mode == Game_Mode::Creative) {
        click_panel_slot(index);
        return;
    }
    const bool is_hotbar_slot = index >= k_panel_hotbar_start;
    Item_Stack& slot = is_hotbar_slot ? m_survival_hotbar[index - k_panel_hotbar_start]
                                      : m_survival_storage[index];
    right_click_stack(slot, m_carried);
}

Item_Stack Inventory::get_craft_slot(size_t index) const {
    return index < k_craft_size ? m_craft_grid[index] : Item_Stack{};
}

Item_Stack Inventory::peek_craft_output() const {
    if (m_mode != Game_Mode::Survival) return {};
    std::array<Item_Stack, 9> grid{};
    grid[0] = m_craft_grid[0];
    grid[1] = m_craft_grid[1];
    grid[3] = m_craft_grid[2];
    grid[4] = m_craft_grid[3];
    const Recipe* recipe = Recipe_Registry::get_instance().match(grid);
    if (!recipe) return {};
    return Item_Stack{recipe->output_type, recipe->output_count};
}

void Inventory::click_craft_slot(size_t index, bool right) {
    if (index >= k_craft_size || m_mode != Game_Mode::Survival) return;
    if (right) right_click_stack(m_craft_grid[index], m_carried);
    else left_click_stack(m_craft_grid[index], m_carried);
}

void Inventory::click_craft_output() {
    const Item_Stack result = peek_craft_output();
    if (result.is_empty()) return;
    if (!m_carried.is_empty() &&
        !(m_carried.type == result.type && m_carried.damage == 0 &&
          m_carried.count + result.count <= max_stack_for(result.type))) {
        return;
    }
    for (Item_Stack& cell : m_craft_grid) {
        if (cell.is_empty()) continue;
        cell.count -= 1;
        if (cell.count <= 0) cell = Item_Stack{};
    }
    if (m_carried.is_empty()) m_carried = result;
    else m_carried.count += result.count;
}

void Inventory::return_craft_to_inventory() {
    for (Item_Stack& cell : m_craft_grid) {
        if (!cell.is_empty()) try_add_stack(cell);
        cell = Item_Stack{};
    }
}

void Inventory::drop_carried_into_inventory() {
    return_craft_to_inventory();
    if (m_carried.is_empty()) {
        m_carried = Item_Stack{};
        return;
    }
    // В Creative курсор — просто копия блока, возвращать её некуда и незачем.
    if (m_mode == Game_Mode::Creative) {
        m_carried = Item_Stack{};
        return;
    }
    // В Survival стак нельзя терять при закрытии панели: раскладываем обратно.
    try_add_stack(m_carried);
    m_carried = Item_Stack{};
}

int Inventory::get_selected_grid_index() const {
    // Активный слот хотбара всегда подсвечивается в нижней строке панели.
    return static_cast<int>(k_panel_hotbar_start) + m_selected_index;
}

Block_Types Inventory::get_selected_block() const {
    return get_hotbar_slot(static_cast<size_t>(m_selected_index)).type;
}

const std::string& Inventory::get_selected_block_name() const {
    static const std::string empty_name = "-";
    const Block_Types block = get_selected_block();
    if (block == Block_Types::Air) return empty_name;
    return get_block_props(block).name;
}

void Inventory::merge_into(std::vector<Item_Stack>& slots, Block_Types type, int& remaining) {
    const int max_stack = max_stack_for(type);
    for (Item_Stack& slot : slots) {
        if (remaining <= 0) return;
        if (slot.type == type && slot.count > 0 && slot.count < max_stack && slot.damage == 0) {
            const int can_take = max_stack - slot.count;
            const int take = std::min(can_take, remaining);
            slot.count += take;
            remaining -= take;
        }
    }
}

void Inventory::fill_empty(std::vector<Item_Stack>& slots, Block_Types type, int& remaining, int damage) {
    const int max_stack = max_stack_for(type);
    for (Item_Stack& slot : slots) {
        if (remaining <= 0) return;
        if (slot.is_empty()) {
            const int take = std::min(max_stack, remaining);
            slot.type = type;
            slot.count = take;
            slot.damage = damage;
            remaining -= take;
        }
    }
}

void Inventory::consume_from(std::vector<Item_Stack>& slots, Block_Types type, int& remaining) {
    for (Item_Stack& slot : slots) {
        if (remaining <= 0) return;
        if (slot.type == type && slot.count > 0) {
            const int take = std::min(slot.count, remaining);
            slot.count -= take;
            remaining -= take;
            if (slot.count <= 0) {
                slot = Item_Stack{};
            }
        }
    }
}

int Inventory::try_add(Block_Types type, int amount) {
    return try_add_stack(Item_Stack{type, amount});
}

Item_Stack Inventory::get_selected_stack() const {
    if (m_mode != Game_Mode::Survival) return {};
    return get_hotbar_slot(static_cast<size_t>(m_selected_index));
}

bool Inventory::damage_selected_tool(int amount) {
    if (m_mode != Game_Mode::Survival) return false;
    Item_Stack& slot = m_survival_hotbar[static_cast<size_t>(m_selected_index)];
    if (slot.is_empty()) return false;
    const int max_durability = get_block_props(slot.type).max_durability;
    if (max_durability <= 0) return false;
    slot.damage += amount;
    if (slot.damage >= max_durability) {
        slot = Item_Stack{};
        return true;
    }
    return false;
}

bool Inventory::consume_selected(int count) {
    if (m_mode != Game_Mode::Survival) return false;
    Item_Stack& slot = m_survival_hotbar[static_cast<size_t>(m_selected_index)];
    if (slot.is_empty() || slot.count < count) return false;
    slot.count -= count;
    if (slot.count <= 0) slot = Item_Stack{};
    return true;
}

int Inventory::try_add_stack(const Item_Stack& stack) {
    const Block_Types type = stack.type;
    const int amount = stack.count;
    if (amount <= 0 || type == Block_Types::Air) return 0;

    int remaining = amount;
    // Сначала пытаемся доложить в уже существующие стаки (хотбар приоритетнее, как в
    // Minecraft), и только потом — в пустые слоты.
    merge_into(m_survival_hotbar, type, remaining);
    merge_into(m_survival_storage, type, remaining);
    fill_empty(m_survival_hotbar, type, remaining, stack.damage);
    fill_empty(m_survival_storage, type, remaining, stack.damage);

    return amount - remaining;
}

bool Inventory::add_resource(Block_Types type, int amount) {
    // Если инвентарь переполнен, то, что не поместилось, теряется — упрощение, которое
    // осталось для путей, где предмет некуда выбросить (возврат стака с курсора при закрытии
    // экрана). Подбор с земли использует try_add и оставляет остаток лежать в мире.
    return try_add(type, amount) > 0;
}

bool Inventory::has_resource(Block_Types type, int amount) const {
    if (amount <= 0) return true;
    int total = 0;
    for (const Item_Stack& s : m_survival_hotbar) if (s.type == type) total += s.count;
    for (const Item_Stack& s : m_survival_storage) if (s.type == type) total += s.count;
    return total >= amount;
}

bool Inventory::remove_resource(Block_Types type, int amount) {
    if (amount <= 0) return true;
    if (!has_resource(type, amount)) return false;

    int remaining = amount;
    consume_from(m_survival_hotbar, type, remaining);
    consume_from(m_survival_storage, type, remaining);
    return true;
}

std::string Inventory::serialize() const {
    std::ostringstream out;
    out << "sel " << m_selected_index << "\n";
    for (size_t i = 0; i < m_creative_hotbar.size(); ++i) {
        out << "c " << i << " " << static_cast<int>(m_creative_hotbar[i]) << "\n";
    }
    for (size_t i = 0; i < m_survival_hotbar.size(); ++i) {
        if (m_survival_hotbar[i].is_empty()) continue;
        out << "h " << i << " " << static_cast<int>(m_survival_hotbar[i].type) << " " << m_survival_hotbar[i].count << "\n";
        if (m_survival_hotbar[i].damage > 0) out << "hd " << i << " " << m_survival_hotbar[i].damage << "\n";
    }
    for (size_t i = 0; i < m_survival_storage.size(); ++i) {
        if (m_survival_storage[i].is_empty()) continue;
        out << "s " << i << " " << static_cast<int>(m_survival_storage[i].type) << " " << m_survival_storage[i].count << "\n";
        if (m_survival_storage[i].damage > 0) out << "sd " << i << " " << m_survival_storage[i].damage << "\n";
    }
    return out.str();
}

void Inventory::deserialize(const std::string& text) {
    std::istringstream in(text);
    std::string tag;
    while (in >> tag) {
        if (tag == "sel") {
            int v = 0; in >> v;
            if (v >= 0 && v < static_cast<int>(k_hotbar_size)) m_selected_index = v;
        } else if (tag == "c") {
            size_t i = 0; int id = 0; in >> i >> id;
            if (i < m_creative_hotbar.size() && id >= 0 && id < static_cast<int>(k_max_block_types)) {
                m_creative_hotbar[i] = static_cast<Block_Types>(id);
            }
        } else if (tag == "h" || tag == "s") {
            size_t i = 0; int id = 0, count = 0; in >> i >> id >> count;
            std::vector<Item_Stack>& list = tag == "h" ? m_survival_hotbar : m_survival_storage;
            if (i < list.size() && id > 0 && id < static_cast<int>(k_max_block_types) && count > 0) {
                list[i] = Item_Stack{static_cast<Block_Types>(id), std::min(count, max_stack_for(static_cast<Block_Types>(id)))};
            }
        } else if (tag == "hd" || tag == "sd") {
            size_t i = 0; int dmg = 0; in >> i >> dmg;
            std::vector<Item_Stack>& list = tag == "hd" ? m_survival_hotbar : m_survival_storage;
            if (i < list.size() && !list[i].is_empty()) list[i].damage = std::max(0, dmg);
        } else {
            std::string rest; std::getline(in, rest);
        }
    }
}

Item_Stack* Inventory::get_panel_slot_mutable(size_t index) {
    if (m_mode != Game_Mode::Survival || index >= k_panel_slot_count) return nullptr;
    return index >= k_panel_hotbar_start ? &m_survival_hotbar[index - k_panel_hotbar_start]
                                         : &m_survival_storage[index];
}

void Inventory::quick_move_panel(size_t index) {
    Item_Stack* slot = get_panel_slot_mutable(index);
    if (!slot || slot->is_empty()) return;
    // Из хранилища — в хотбар, из хотбара — в хранилище.
    std::vector<Item_Stack>& target = index >= k_panel_hotbar_start ? m_survival_storage : m_survival_hotbar;
    int remaining = slot->count;
    merge_into(target, slot->type, remaining);
    fill_empty(target, slot->type, remaining, slot->damage);
    if (remaining <= 0) *slot = Item_Stack{};
    else slot->count = remaining;
}

void Inventory::quick_move_craft_slot(size_t index) {
    if (index >= k_craft_size || m_mode != Game_Mode::Survival) return;
    Item_Stack& cell = m_craft_grid[index];
    if (cell.is_empty()) return;
    const int added = try_add_stack(cell);
    cell.count -= added;
    if (cell.count <= 0) cell = Item_Stack{};
}

void Inventory::quick_craft_output() {
    for (int guard = 0; guard < 64; ++guard) {
        const Item_Stack result = peek_craft_output();
        if (result.is_empty()) return;
        const int added = try_add(result.type, result.count);
        if (added < result.count) {          // не влезло целиком — откатываем и выходим
            if (added > 0) remove_resource(result.type, added);
            return;
        }
        for (Item_Stack& cell : m_craft_grid) {
            if (cell.is_empty()) continue;
            if (--cell.count <= 0) cell = Item_Stack{};
        }
    }
}
