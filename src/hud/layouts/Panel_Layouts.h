#ifndef OPTICRAFT_PANEL_LAYOUTS_H
#define OPTICRAFT_PANEL_LAYOUTS_H

#include <cstddef>

#include "hud/Hud.h"

// ============================================================================
//  Раскладки панелей со слотами (инвентарь, сундук, верстак, печь)
// ----------------------------------------------------------------------------
//  Чистая геометрия в GUI-пикселях: никакой игровой логики и никаких данных слотов.
//  Game сопоставляет Hud_Panel_Slot::id с содержимым (см. Panel_Slot_Ids) и сам заполняет
//  параллельный массив Hud_Inventory_Slot. Новый контейнер = новая функция здесь, без
//  правок в Game.cpp, кроме сопоставления id.
// ============================================================================
namespace Panel_Layouts {

    // --- общие размеры ---------------------------------------------------------
    constexpr float k_slot_pitch = 18.0f;        // шаг сетки слотов
    constexpr float k_panel_width = 176.0f;
    constexpr float k_panel_margin = 8.0f;       // отступ от края панели до первого слота
    constexpr int k_slots_per_row = 9;

    // --- идентификаторы слотов (Hud_Panel_Slot::id) --------------------------------
    //  0 .. Inventory::k_panel_slot_count-1 (+ смещение) — слоты игрока и содержимое контейнера.
    namespace Panel_Slot_Ids {
        constexpr int k_inventory_craft_base = 100;   // 100..103 — сетка 2x2 в инвентаре
        constexpr int k_inventory_craft_count = 4;
        constexpr int k_inventory_craft_output = 200; // результат крафта 2x2
        constexpr int k_container_output = -2;        // результат верстака / выход печи в сессии
    }

    // Слоты игрока: 3 ряда хранилища (если показано) и ряд хотбара. id = id_offset + индекс
    // в Inventory (хранилище 0..26, хотбар — с Inventory::k_panel_hotbar_start).
    // hotbar_start — этот индекс, передаётся снаружи, чтобы раскладка не зависела от Inventory.
    void append_player_slots(Hud_Panel_Layout& layout, float x, float y_storage, float y_hotbar,
                             bool storage_visible, int id_offset, int hotbar_start);

    Hud_Panel_Layout inventory_creative(size_t page_count, size_t current_page, int hotbar_start);
    Hud_Panel_Layout inventory_survival(int hotbar_start);

    // Контейнеры. player_id_offset — число слотов самого контейнера (и парной половины сундука).
    Hud_Panel_Layout chest(size_t slot_count, bool show_player_storage, int player_id_offset, int hotbar_start);
    Hud_Panel_Layout crafting_table(size_t slot_count, bool show_player_storage, int player_id_offset, int hotbar_start);
    // flame/arrow — заполненность индикаторов 0..1.
    Hud_Panel_Layout furnace(size_t slot_count, float flame, float arrow, bool show_player_storage,
                             int player_id_offset, int hotbar_start);

} // namespace Panel_Layouts

#endif
