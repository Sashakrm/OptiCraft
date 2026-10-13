#ifndef OPTICRAFT_HUD_H
#define OPTICRAFT_HUD_H

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include "rendering/Shader.h"
#include "rendering/atlas/Texture_Atlas.h"

// ============================================================================
//  Интерфейс OptiCraft
// ----------------------------------------------------------------------------
//  Весь интерфейс рисуется из отдельного атласа "ui_main" (assets/atlases/ui_main.atlas.json,
//  генерируется tools/gen_ui_atlas.py) в «GUI-пикселях», как в Minecraft: вёрстка задаётся в
//  маленьких координатах (хотбар 182x22, слот 18x18), а на экране всё умножается на ЦЕЛЫЙ
//  масштаб (см. Hud::get_gui_scale). Поэтому пиксель-арт остаётся чётким, а вёрстка одинакова
//  на любом разрешении.
//  Все функции get_*/..._at — статические и считают ровно ту же геометрию, что и рисование:
//  Game использует их для попадания курсора, поэтому клики не расходятся с картинкой.
// ============================================================================

struct Hud_Debug_Stats {
    float fps;
    float frame_time_ms;
    glm::vec3 player_position;
    unsigned int seed;
    std::string generation_folder;
    size_t chunks_loaded;
    size_t chunks_rendered;
    size_t triangles_rendered;
    size_t sections_rendered;
    size_t sections_culled;
    bool is_flying; // "призрак" (ghost/noclip-полёт) vs "странник" (ходит по земле)
    float day_night_normalized_time; // [0,1) — см. Day_Night_Cycle::get_normalized_time
    std::string biome_name; // текущий биом под игроком, см. World_Generator::get_biome_name
};

struct Hud_Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// Один слот инвентаря/хотбара для отрисовки: иконка (координаты UV в текстурном атласе) + имя.
struct Hud_Inventory_Slot {
    glm::vec4 icon_uv; // (u0, v0, u1, v1), см. Texture_Atlas::get_uv_coords — плоская иконка
    std::string name;
    int count = 0;     // >1 — рисуем число поверх иконки (стак ресурсов в Survival)
    bool empty = false; // true — пустой слот (Survival: ещё не собрано), иконка не рисуется
    float durability = -1.0f; // 0..1 остаток прочности инструмента; < 0 — не инструмент или целый (полоска не рисуется)
    // Блок рисуется как изометрический куб с тремя гранями (как в Minecraft); предметы и
    // растения («крестики») остаются плоскими и используют icon_uv.
    bool as_cube = false;
    glm::vec4 uv_cube_top{0.0f};
    glm::vec4 uv_cube_left{0.0f};   // левая видимая грань (у печи/сундука здесь лицевая сторона)
    glm::vec4 uv_cube_right{0.0f};
};

// Строка чата: текст, ошибка ли это (рисуется красным) и сколько секунд ещё показывать её
// при закрытом чате. При открытом чате видна вся недавняя история независимо от таймера.
struct Hud_Chat_Line {
    std::string text;
    bool is_error = false;
    float time_left = 0.0f;
};

// ---------------------------------------------------------------------------
//  Экраны с кнопками (главное меню, пауза, настройки, выбор/создание мира)
// ---------------------------------------------------------------------------
enum class Hud_Widget_Kind { Button, Slider, Field };

struct Hud_Widget {
    Hud_Widget_Kind kind = Hud_Widget_Kind::Button;
    std::string label;        // Button/Slider: надпись; Field: подпись НАД полем
    std::string text;         // Field: введённый текст
    std::string placeholder;  // Field: серая подсказка, пока поле пустое
    std::string value_text;   // Slider: что написано после ":" ("8 chunks")
    float value = 0.0f;       // Slider: 0..1
    bool enabled = true;
    bool focused = false;     // Field: рамка-подсветка и мигающий курсор

    static Hud_Widget button(std::string label, bool enabled = true) {
        Hud_Widget w; w.label = std::move(label); w.enabled = enabled; return w;
    }
    static Hud_Widget slider(std::string label, std::string value_text, float value) {
        Hud_Widget w; w.kind = Hud_Widget_Kind::Slider; w.label = std::move(label);
        w.value_text = std::move(value_text); w.value = value; return w;
    }
    static Hud_Widget field(std::string caption, std::string text, std::string placeholder, bool focused) {
        Hud_Widget w; w.kind = Hud_Widget_Kind::Field; w.label = std::move(caption);
        w.text = std::move(text); w.placeholder = std::move(placeholder); w.focused = focused; return w;
    }
};

enum class Hud_Screen_Layout {
    Centered,     // колонка виджетов по центру экрана (меню, настройки, создание мира)
    Bottom_Grid   // виджеты внизу, между заголовком и ними — список (выбор мира)
};

struct Hud_Screen {
    std::string title;
    std::string subtitle;
    std::vector<Hud_Widget> widgets;
    int columns = 1;
    bool last_full_width = false;   // последний виджет («Готово») на всю ширину под сеткой
    bool show_logo = false;         // логотип вместо текстового заголовка
    bool dirt_background = true;    // true — земляные плитки (вне игры); false — затемнённый мир за экраном
    Hud_Screen_Layout layout = Hud_Screen_Layout::Centered;
    std::string footer;             // мелкий текст внизу слева (версия)
};

// Запись в списке миров.
struct Hud_World_Entry {
    std::string name;
    std::string detail;   // "my_world (2026-10-03 15:10)"
    std::string detail2;  // "Survival Mode, Classic, seed 1327"
};

// ---------------------------------------------------------------------------
//  Универсальная панель слотов (инвентарь, сундук, верстак, печь)
// ---------------------------------------------------------------------------
struct Hud_Panel_Slot {
    glm::vec2 pos;     // левый верхний угол РАМКИ слота (18x18), GUI-пиксели от угла панели
    int id = 0;        // что вернуть вызывающему коду при клике/наведении
    bool output = false; // слот-результат (крупнее, без подложки-углубления)
};

struct Hud_Panel_Label {
    std::string text;
    glm::vec2 pos;
};

struct Hud_Panel_Progress {
    glm::vec2 pos;
    float fraction = 0.0f;
    bool flame = false;   // true — огонь печи (заполняется снизу вверх), false — стрелка (слева направо)
};

struct Hud_Panel_Layout {
    glm::vec2 size{176.0f, 166.0f};
    std::vector<Hud_Panel_Slot> slots;
    std::vector<Hud_Panel_Label> labels;
    std::vector<Hud_Panel_Progress> progress;
    bool player_preview = false;
    Hud_Rect preview_rect;           // в GUI-пикселях панели
    // Листание страниц Creative: две кнопки и подпись между ними.
    bool page_controls = false;
    glm::vec2 page_prev_pos{0, 0};
    glm::vec2 page_next_pos{0, 0};
    std::string page_text;
    glm::vec2 page_text_pos{0, 0};
};

class Hud {
public:
    Hud();
    ~Hud();

    bool initialize();

    // --- масштаб и вёрстка --------------------------------------------------
    // 0 = авто (по размеру окна), иначе фиксированный масштаб 1..4 из настроек.
    static void set_gui_scale_setting(int setting);
    static int get_gui_scale_setting();
    static int get_gui_scale(int window_width, int window_height);

    static std::vector<Hud_Rect> get_widget_rects(int window_width, int window_height, const Hud_Screen& screen);
    // Индекс виджета под курсором (-1 — нет). Выключенные виджеты не считаются.
    static int get_widget_at(int window_width, int window_height, const Hud_Screen& screen, float mx, float my);
    // Для слайдера: значение 0..1 по X курсора.
    static float get_slider_value_at(const Hud_Rect& rect, float mx);
    // Область списка миров на экране Bottom_Grid и геометрия строки.
    static Hud_Rect get_world_list_rect(int window_width, int window_height, const Hud_Screen& screen);
    static float get_world_row_height(int window_width, int window_height);
    static int get_world_row_at(int window_width, int window_height, const Hud_Screen& screen,
                                size_t entry_count, float scroll_px, float mx, float my);
    static float get_world_list_max_scroll(int window_width, int window_height, const Hud_Screen& screen, size_t entry_count);

    static float get_panel_scale(int window_width, int window_height, const Hud_Panel_Layout& layout);
    static Hud_Rect get_panel_rect(int window_width, int window_height, const Hud_Panel_Layout& layout);
    // Индекс слота панели (в layout.slots) под курсором или -1.
    static int get_panel_slot_at(int window_width, int window_height, const Hud_Panel_Layout& layout, float mx, float my);
    // 0 — «назад», 1 — «вперёд», -1 — мимо.
    static int get_panel_page_button_at(int window_width, int window_height, const Hud_Panel_Layout& layout, float mx, float my);

    // --- экраны -------------------------------------------------------------
    void draw_menu_screen(int window_width, int window_height, const Hud_Screen& screen,
                          int hovered_widget,
                          const std::vector<Hud_World_Entry>* worlds = nullptr,
                          int selected_world = -1, float scroll_px = 0.0f,
                          bool caret_visible = true);
    void draw_loading_screen(int window_width, int window_height, const std::string& title,
                             const std::string& status, float progress);
    void draw_panel_screen(int window_width, int window_height,
                           const Texture_Atlas& block_atlas,
                           const Hud_Panel_Layout& layout,
                           const std::vector<Hud_Inventory_Slot>& slots,
                           int hovered_slot,
                           const Hud_Inventory_Slot* carried_slot,
                           float cursor_x, float cursor_y,
                           const std::string& title);

    // --- игровой HUD ---------------------------------------------------------
    void draw_crosshair(int window_width, int window_height, bool target_hit);
    // Сердца и куриные ножки над хотбаром (только Survival).
    void draw_player_status(int window_width, int window_height,
                            int health, int max_health,
                            int food, int max_food);
    // Тонкая полоска под прицелом: прогресс ломания блока / поедания еды (fraction 0..1).
    void draw_action_progress(int window_width, int window_height, float fraction, const glm::vec4& color);
    void draw_hotbar(int window_width, int window_height,
                     const Texture_Atlas& block_atlas,
                     const std::vector<Hud_Inventory_Slot>& slots,
                     int selected_index, bool survival);
    void draw_block_highlight(const glm::ivec3& block_pos,
                              const glm::mat4& view,
                              const glm::mat4& projection);
    void draw_vignette(int window_width, int window_height);
    void draw_underwater_overlay(int window_width, int window_height);
    void draw_debug_overlay(int window_width, int window_height, const Hud_Debug_Stats& stats);
    void draw_chat_overlay(int window_width, int window_height,
                           const std::vector<Hud_Chat_Line>& log_lines,
                           const std::string& input_line,
                           bool chat_open,
                           bool caret_visible);

    // Ширина строки в экранных пикселях при данном целом масштабе шрифта.
    float get_text_width(const std::string& text, float scale) const;

private:
    struct Sprite {
        glm::vec4 uv{0.0f};     // (u0, v0, u1, v1)
        glm::vec4 rect{0.0f};   // пиксельный (x, y, w, h) в PNG
    };
    struct Ui_Vertex { float x, y, u, v, r, g, b, a; };

    // --- пакетный рендер квадов из UI-атласа ---
    void begin_ui(int window_width, int window_height);
    void end_ui();
    void flush_ui();
    const Sprite& sprite(const std::string& name);
    void quad(float x, float y, float w, float h, const glm::vec4& uv, const glm::vec4& color);
    void draw_sprite(const std::string& name, float x, float y, float w, float h,
                     const glm::vec4& color = glm::vec4(1.0f));
    // 9-slice: border — ширина угла в пикселях СПРАЙТА (на экране умножается на scale).
    void draw_nine_slice(const std::string& name, float x, float y, float w, float h,
                         float border, float scale, const glm::vec4& color = glm::vec4(1.0f));
    void draw_rect(float x, float y, float width, float height, const glm::vec4& color);
    void draw_gradient(float x, float y, float w, float h, const glm::vec4& top, const glm::vec4& bottom);
    // Часть спрайта: fx — доля ширины слева, fy — доля высоты СНИЗУ (для стрелки и огня печи).
    void draw_sprite_partial(const std::string& name, float x, float y, float w, float h, float fx, float fy);
    void draw_text(const std::string& text, float x, float y, float scale,
                   const glm::vec4& color = glm::vec4(1.0f), bool shadow = true);
    void draw_text_centered(const std::string& text, float center_x, float y, float scale,
                            const glm::vec4& color = glm::vec4(1.0f), bool shadow = true);
    void draw_tiled_background(float scale);
    // Иконка слота: изометрический куб или плоский спрайт. Вызывать между begin/end_icon_batch.
    void draw_slot_icon(const Hud_Inventory_Slot& slot, float x, float y, float size);
    // Произвольный четырёхугольник (параллелограмм) с текстурой: углы по часовой от левого верхнего.
    void quad_skewed(const glm::vec2& tl, const glm::vec2& tr, const glm::vec2& br, const glm::vec2& bl,
                     const glm::vec4& uv, const glm::vec4& color);
    void begin_icon_batch(const Texture_Atlas& atlas);
    void end_icon_batch();
    void draw_slot_contents(const std::vector<Hud_Inventory_Slot>& slots,
                            const std::vector<glm::vec2>& icon_positions,
                            float icon_size, float scale, const Texture_Atlas& atlas);
    void draw_widget(const Hud_Widget& widget, const Hud_Rect& rect, bool hovered,
                     float scale, bool caret_visible);

    Shader* m_line_shader = nullptr;
    Shader* m_vignette_shader = nullptr;
    Shader* m_ui_shader = nullptr;
    const Texture_Atlas* m_ui_atlas = nullptr;
    unsigned int m_highlight_vao = 0;
    unsigned int m_highlight_vbo = 0;
    unsigned int m_fullscreen_vao = 0;
    unsigned int m_fullscreen_vbo = 0;
    unsigned int m_ui_vao = 0;
    unsigned int m_ui_vbo = 0;
    int m_window_width = 1;
    int m_window_height = 1;

    std::unordered_map<std::string, Sprite> m_sprites;
    std::unordered_map<char32_t, Sprite> m_glyphs;   // ключ — кодовая точка Unicode
    std::vector<Ui_Vertex> m_batch;
    // Какой атлас привязывать при flush_ui(); nullptr — UI-атлас. Нужен для пакета иконок блоков.
    const Texture_Atlas* m_batch_atlas = nullptr;

    // Для анимаций HUD: вспышка сердец при уроне и подпись выбранного предмета.
    int m_last_health = -1;
    double m_heart_flash_until = 0.0;
    int m_last_selected_slot = -1;
    double m_selected_name_until = 0.0;
};

#endif
