//
// Created by noktemor on 26.08.2026.
//

#ifndef OPTICRAFT_INVENTORY_H
#define OPTICRAFT_INVENTORY_H

#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include "world/blocks/Block_Types.h"

// Режим игры. Creative — бесконечный доступ ко всем блокам (как было раньше), Survival —
// хотбар и инвентарь хранят только то, что реально собрано (стаки ресурсов, как в
// обычном Minecraft), плюс урон от падения (см. Player).
enum class Game_Mode : uint8_t {
    Creative,
    Survival
};

// Один слот со стаком одинаковых блоков (нужен только для Survival — в Creative счётчик
// не нужен, доступ к блокам бесконечен).
struct Item_Stack {
    Block_Types type = Block_Types::Air;
    int count = 0;
    // Сколько единиц прочности уже потрачено (только у инструментов; 0 = целый).
    int damage = 0;

    bool is_empty() const { return count <= 0 || type == Block_Types::Air; }
};

// Размер стака: инструменты (есть прочность) не стакаются, всё остальное — до 64.
inline int max_stack_for(Block_Types type) {
    return get_block_props(type).max_durability > 0 ? 1 : 64;
}

class Inventory {
public:
    Inventory();

    void set_mode(Game_Mode mode) { m_mode = mode; }
    Game_Mode get_mode() const { return m_mode; }

    // Переключение АКТИВНОГО СЛОТА ХОТБАРА (0..k_hotbar_size-1) — клавиши [ ] и 1-9.
    void select_next();
    void select_previous();
    void select_index(size_t index);
    int get_selected_index() const { return m_selected_index; }

    // Блок в текущем активном слоте хотбара — то, чем игрок сейчас ставит блоки.
    Block_Types get_selected_block() const;
    const std::string& get_selected_block_name() const;

    // Хотбар (нижняя полоса, всегда на экране): k_hotbar_size слотов.
    // Creative — каждый слот содержит один тип блока (см. pick_grid_slot), запас бесконечен.
    // Survival — каждый слот содержит реальный собранный стак (может быть пустым).
    static constexpr size_t k_hotbar_size = 9;
    size_t get_hotbar_slot_count() const { return k_hotbar_size; }
    Item_Stack get_hotbar_slot(size_t index) const;

    // --- Панель инвентаря (`E`): сетка 9 x 4 --------------------------------
    // Единая сетка на 36 слотов, где НИЖНЯЯ строка (индексы 27..35) — это тот же
    // самый хотбар, что виден на экране во время игры. Раньше сетка и хотбар были
    // двумя независимыми списками, и перетащить предмет в хотбар можно было только
    // через "активный слот", что и ощущалось как сломанный инвентарь.
    static constexpr size_t k_panel_columns = 9;
    static constexpr size_t k_panel_rows = 4;
    static constexpr size_t k_panel_storage_slots = k_panel_columns * (k_panel_rows - 1); // 27
    static constexpr size_t k_panel_slot_count = k_panel_columns * k_panel_rows;          // 36
    // Индекс, с которого в панели начинается хотбар.
    static constexpr size_t k_panel_hotbar_start = k_panel_storage_slots;

    size_t get_panel_slot_count() const { return k_panel_slot_count; }
    size_t get_panel_columns() const { return k_panel_columns; }
    size_t get_panel_rows() const { return k_panel_rows; }
    Item_Stack get_panel_slot(size_t index) const;

    // Клик по слоту панели. Модель одна для обоих режимов и обеих строк — как
    // курсор в Minecraft: пустая рука берёт стак из слота, занятая кладёт его
    // обратно (или меняет местами с тем, что там лежало).
    // Creative: верхние строки — бесконечный источник блоков, взять оттуда можно
    // сколько угодно раз, а положить обратно значит просто выбросить.
    void click_panel_slot(size_t index);

    // Стак "в руке" (на курсоре). Рисуется у курсора, пока панель открыта.
    Item_Stack get_carried_stack() const { return m_carried; }
    bool has_carried_stack() const { return !m_carried.is_empty(); }
    // Позволяет внешнему коду (см. Container_Session) временно "одолжить" курсор
    // инвентаря, чтобы прогнать клик по хотбару через click_panel_slot и получить
    // тот же самый merge/swap, не дублируя его логику у контейнеров.
    void set_carried_stack(Item_Stack stack) { m_carried = stack; }
    // Сохранение содержимого в простой текст (хотбар, хранилище, выбранный слот) и обратно.
    std::string serialize() const;
    void deserialize(const std::string& text);
    // Возврат стака в инвентарь при закрытии панели, чтобы он не потерялся.
    void drop_carried_into_inventory();

    // --- Страницы Creative ---------------------------------------------------
    // Блоков в реестре больше, чем 27 верхних слотов, поэтому Creative-сетка
    // листается страницами.
    size_t get_page_count() const;
    size_t get_current_page() const { return m_creative_page; }
    void next_page();
    void previous_page();

    // ПКМ по слоту: Survival — взять половину стака / положить один предмет; Creative — как ЛКМ.
    void right_click_panel_slot(size_t index);

    // --- Крафт 2x2 в окне инвентаря (только Survival) -------------------------
    // Ячейки 0..3 идут по строкам: [0 1 / 2 3]. Результат считается на лету через
    // Recipe_Registry (2x2 кладётся в левый верхний угол сетки 3x3).
    static constexpr size_t k_craft_size = 4;
    Item_Stack get_craft_slot(size_t index) const;
    Item_Stack peek_craft_output() const;
    void click_craft_slot(size_t index, bool right = false);
    void click_craft_output();
    // Быстрый перенос Shift+ЛКМ (Survival): хранилище <-> хотбар, ячейка сетки -> в сумку,
    // результат крафта -> крафтит сколько влезет.
    void quick_move_panel(size_t index);
    void quick_move_craft_slot(size_t index);
    void quick_craft_output();
    // --- Броня (только Survival) ----------------------------------------------------------
    // Четыре слота: index 0 — шлем, 1 — нагрудник, 2 — поножи, 3 — ботинки (= Armor_Slot - 1).
    // Надеть можно только предмет нужного слота (см. blocks.json, "armor_slot").
    static constexpr size_t k_armor_count = k_armor_slot_count;
    Item_Stack get_armor_slot(size_t index) const;
    void click_armor_slot(size_t index, bool right = false);
    // Очки защиты надетой брони (сумма по надетым деталям).
    int armor_points() const;
    // Износ брони от удара: каждая надетая деталь теряет amount прочности, сломанная пропадает.
    void damage_armor(int amount = 1);

    // Прямой доступ к слоту панели (nullptr в Creative или вне диапазона) — для контейнеров.
    Item_Stack* get_panel_slot_mutable(size_t index);
    // Вернуть содержимое сетки в инвентарь (при закрытии окна).
    void return_craft_to_inventory();

    // Позиция текущего выбранного блока хотбара внутри панели — для подсветки.
    int get_selected_grid_index() const;

    // Survival: добавить/списать собранный ресурс — вызывается из Block_Interaction при
    // добыче/установке блока.
    bool add_resource(Block_Types type, int amount = 1);
    // Как add_resource, но возвращает, СКОЛЬКО штук реально влезло (0..amount). Нужен подбору
    // выпавших предметов: если инвентарь почти полон, на земле остаётся остаток стака.
    int try_add(Block_Types type, int amount);
    // То же для готового стака: сохраняет прочность инструмента.
    int try_add_stack(const Item_Stack& stack);
    // Предмет в активном слоте хотбара (Survival; в Creative — пустой).
    Item_Stack get_selected_stack() const;
    // Тратит прочность инструмента в активном слоте (Survival). true — инструмент сломался.
    bool damage_selected_tool(int amount = 1);
    // Списывает count штук из активного слота хотбара (Survival).
    bool consume_selected(int count = 1);
    bool has_resource(Block_Types type, int amount = 1) const;
    bool remove_resource(Block_Types type, int amount = 1);

private:
    Game_Mode m_mode = Game_Mode::Creative;
    int m_selected_index = 0; // Индекс активного слота хотбара — общий для обоих режимов
    Item_Stack m_carried;     // Стак на курсоре, пока открыта панель инвентаря
    size_t m_creative_page = 0;

    // Creative
    std::vector<Block_Types> m_all_blocks;      // Все зарегистрированные блоки, кроме Air
    std::vector<Block_Types> m_creative_hotbar; // k_hotbar_size, изначально первые блоки реестра

    // Survival
    static constexpr size_t k_survival_storage_size = k_panel_storage_slots; // 3x9 верхних строк
    static constexpr int k_max_stack = 64;                // как в Minecraft
    std::vector<Item_Stack> m_survival_hotbar;   // k_hotbar_size, изначально пусто
    std::vector<Item_Stack> m_survival_storage;  // k_survival_storage_size, изначально пусто
    Item_Stack m_craft_grid[k_craft_size];       // сетка крафта 2x2 в окне инвентаря
    std::array<Item_Stack, k_armor_count> m_armor{}; // надетая броня (Survival)

    // Блок, который должен показываться в слоте панели index для Creative
    // (с учётом текущей страницы). Air — слот за пределами списка блоков.
    Block_Types creative_block_at(size_t panel_index) const;

    static void left_click_stack(Item_Stack& slot, Item_Stack& carried);
    static void right_click_stack(Item_Stack& slot, Item_Stack& carried);

    static void merge_into(std::vector<Item_Stack>& slots, Block_Types type, int& remaining);
    static void fill_empty(std::vector<Item_Stack>& slots, Block_Types type, int& remaining, int damage = 0);
    static void consume_from(std::vector<Item_Stack>& slots, Block_Types type, int& remaining);
};

#endif //OPTICRAFT_INVENTORY_H
