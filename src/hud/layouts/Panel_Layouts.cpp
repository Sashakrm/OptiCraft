#include "hud/layouts/Panel_Layouts.h"

#include <string>

#include "utils/text/Localization.h"

namespace Panel_Layouts {

    namespace {
        // --- инвентарь: Creative ---
        // Сверху палитра блоков (3 ряда по 9), под ней листалка страниц, ниже — тот же
        // хранилище + хотбар, что и в Survival: предметы из палитры попадают в них.
        constexpr float k_creative_height = 166.0f;
        constexpr float k_creative_palette_y = 18.0f;
        constexpr float k_creative_storage_y = 84.0f;
        constexpr float k_creative_hotbar_y = 142.0f;
        constexpr glm::vec2 k_creative_page_prev{8.0f, 76.0f};
        constexpr glm::vec2 k_creative_page_next{154.0f, 76.0f};
        constexpr glm::vec2 k_creative_page_text{88.0f, 79.0f};

        // --- инвентарь: Survival ---
        constexpr float k_survival_height = 166.0f;
        constexpr glm::vec4 k_player_preview{32.0f, 8.0f, 44.0f, 70.0f};  // x, y, w, h (правее столбца брони)
        constexpr glm::vec2 k_armor_column{8.0f, 8.0f};                   // столбец слотов брони, шаг k_slot_pitch
        constexpr glm::vec2 k_craft_label{80.0f, 6.0f};
        constexpr glm::vec2 k_craft_grid_origin{80.0f, 18.0f};            // сетка 2x2, шаг k_slot_pitch
        constexpr glm::vec2 k_craft_arrow{119.0f, 28.0f};
        constexpr glm::vec2 k_craft_output{150.0f, 27.0f};
        constexpr float k_survival_storage_y = 84.0f;
        constexpr float k_survival_hotbar_y = 142.0f;

        // --- контейнеры ---
        constexpr float k_container_first_row_y = 18.0f;
        constexpr float k_container_storage_label_gap = 11.0f;   // «Inventory» над хранилищем
        constexpr float k_container_hotbar_offset = 58.0f;       // хранилище (54) + зазор
        constexpr float k_container_bottom_pad = 26.0f;
        constexpr float k_container_default_storage_y = 84.0f;
        constexpr float k_chest_storage_extra = 30.0f;           // первый ряд (18) + заголовок «Inventory» и зазор

        constexpr glm::vec2 k_table_grid_origin{30.0f, 17.0f};   // сетка 3x3
        constexpr glm::vec2 k_table_arrow{90.0f, 34.0f};
        constexpr glm::vec2 k_table_output{124.0f, 35.0f};

        constexpr glm::vec2 k_furnace_input{56.0f, 17.0f};
        constexpr glm::vec2 k_furnace_fuel{56.0f, 53.0f};
        constexpr glm::vec2 k_furnace_output{116.0f, 35.0f};
        constexpr glm::vec2 k_furnace_flame{57.0f, 36.0f};
        constexpr glm::vec2 k_furnace_arrow{80.0f, 35.0f};
        constexpr size_t k_furnace_slot_count = 3;
        constexpr size_t k_furnace_output_index = 2;

        // Общий хвост контейнера: подпись «Inventory», слоты игрока, высота панели.
        void finish_container(Hud_Panel_Layout& layout, float y_storage, bool show_player_storage,
                              int player_id_offset, int hotbar_start) {
            const float y_hotbar = show_player_storage ? y_storage + k_container_hotbar_offset : y_storage;
            layout.size = {k_panel_width, y_hotbar + k_container_bottom_pad};
            if (show_player_storage) {
                layout.labels.push_back({tr("panel.inventory"),
                                         {k_panel_margin, y_storage - k_container_storage_label_gap}});
            }
            append_player_slots(layout, k_panel_margin, y_storage, y_hotbar, show_player_storage,
                                player_id_offset, hotbar_start);
        }
    }

    void append_player_slots(Hud_Panel_Layout& layout, float x, float y_storage, float y_hotbar,
                             bool storage_visible, int id_offset, int hotbar_start) {
        if (storage_visible) {
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < k_slots_per_row; ++c) {
                    const int index = r * k_slots_per_row + c;
                    layout.slots.push_back({{x + k_slot_pitch * static_cast<float>(c),
                                             y_storage + k_slot_pitch * static_cast<float>(r)},
                                            id_offset + index, false});
                }
            }
        }
        for (int c = 0; c < k_slots_per_row; ++c) {
            layout.slots.push_back({{x + k_slot_pitch * static_cast<float>(c), y_hotbar},
                                    id_offset + hotbar_start + c, false});
        }
    }

    Hud_Panel_Layout inventory_creative(size_t page_count, size_t current_page, int hotbar_start) {
        Hud_Panel_Layout layout;
        layout.size = {k_panel_width, k_creative_height};
        if (page_count > 1) {
            layout.page_controls = true;
            layout.page_prev_pos = k_creative_page_prev;
            layout.page_next_pos = k_creative_page_next;
            layout.page_text = tr("panel.page", {std::to_string(current_page + 1), std::to_string(page_count)});
            layout.page_text_pos = k_creative_page_text;
        }
        for (int i = 0; i < Panel_Slot_Ids::k_creative_palette_count; ++i) {
            layout.slots.push_back({{k_panel_margin + k_slot_pitch * static_cast<float>(i % k_slots_per_row),
                                     k_creative_palette_y + k_slot_pitch * static_cast<float>(i / k_slots_per_row)},
                                    Panel_Slot_Ids::k_creative_palette_base + i, false});
        }
        append_player_slots(layout, k_panel_margin, k_creative_storage_y, k_creative_hotbar_y, true, 0, hotbar_start);
        return layout;
    }

    Hud_Panel_Layout inventory_survival(int hotbar_start) {
        Hud_Panel_Layout layout;
        layout.size = {k_panel_width, k_survival_height};
        layout.player_preview = true;
        layout.preview_rect = {k_player_preview.x, k_player_preview.y, k_player_preview.z, k_player_preview.w};
        layout.labels.push_back({tr("panel.crafting"), k_craft_label});
        for (int i = 0; i < Panel_Slot_Ids::k_inventory_craft_count; ++i) {
            layout.slots.push_back({{k_craft_grid_origin.x + k_slot_pitch * static_cast<float>(i % 2),
                                     k_craft_grid_origin.y + k_slot_pitch * static_cast<float>(i / 2)},
                                    Panel_Slot_Ids::k_inventory_craft_base + i, false});
        }
        layout.progress.push_back({k_craft_arrow, 0.0f, false});
        layout.slots.push_back({k_craft_output, Panel_Slot_Ids::k_inventory_craft_output, true});
        for (int i = 0; i < Panel_Slot_Ids::k_inventory_armor_count; ++i) {
            layout.slots.push_back({{k_armor_column.x, k_armor_column.y + k_slot_pitch * static_cast<float>(i)},
                                    Panel_Slot_Ids::k_inventory_armor_base + i, false});
        }
        append_player_slots(layout, k_panel_margin, k_survival_storage_y, k_survival_hotbar_y, true, 0, hotbar_start);
        return layout;
    }

    Hud_Panel_Layout chest(size_t slot_count, bool show_player_storage, int player_id_offset, int hotbar_start) {
        Hud_Panel_Layout layout;
        const size_t rows = (slot_count + k_slots_per_row - 1) / k_slots_per_row;
        for (size_t k = 0; k < slot_count; ++k) {
            layout.slots.push_back({{k_panel_margin + k_slot_pitch * static_cast<float>(k % k_slots_per_row),
                                     k_container_first_row_y + k_slot_pitch * static_cast<float>(k / k_slots_per_row)},
                                    static_cast<int>(k), false});
        }
        const float y_storage = static_cast<float>(rows) * k_slot_pitch + k_chest_storage_extra;
        finish_container(layout, y_storage, show_player_storage, player_id_offset, hotbar_start);
        return layout;
    }

    Hud_Panel_Layout crafting_table(size_t slot_count, bool show_player_storage, int player_id_offset, int hotbar_start) {
        Hud_Panel_Layout layout;
        for (size_t k = 0; k < slot_count; ++k) {
            layout.slots.push_back({{k_table_grid_origin.x + k_slot_pitch * static_cast<float>(k % 3),
                                     k_table_grid_origin.y + k_slot_pitch * static_cast<float>(k / 3)},
                                    static_cast<int>(k), false});
        }
        layout.progress.push_back({k_table_arrow, 0.0f, false});
        layout.slots.push_back({k_table_output, Panel_Slot_Ids::k_container_output, true});
        finish_container(layout, k_container_default_storage_y, show_player_storage, player_id_offset, hotbar_start);
        return layout;
    }

    Hud_Panel_Layout furnace(size_t slot_count, float flame, float arrow, bool show_player_storage,
                             int player_id_offset, int hotbar_start) {
        Hud_Panel_Layout layout;
        const glm::vec2 positions[k_furnace_slot_count] = {k_furnace_input, k_furnace_fuel, k_furnace_output};
        for (size_t k = 0; k < slot_count && k < k_furnace_slot_count; ++k) {
            layout.slots.push_back({positions[k], static_cast<int>(k), k == k_furnace_output_index});
        }
        layout.progress.push_back({k_furnace_flame, flame, true});
        layout.progress.push_back({k_furnace_arrow, arrow, false});
        finish_container(layout, k_container_default_storage_y, show_player_storage, player_id_offset, hotbar_start);
        return layout;
    }

} // namespace Panel_Layouts
