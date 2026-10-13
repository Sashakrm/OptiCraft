#include "hud/Hud.h"
#include "utils/text/Localization.h"
#include <glad/gl.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// ============================================================================
//  Экраны с кнопками: меню, настройки, выбор и создание мира, загрузка.
//  Вся вёрстка — в GUI-пикселях; get_widget_rects и рисование используют одну геометрию.
// ============================================================================
namespace {
    constexpr float k_column_width = 200.0f;
    constexpr float k_button_h = 20.0f;
    constexpr float k_gap = 4.0f;
    constexpr float k_field_caption_h = 12.0f;
    constexpr float k_world_row_h = 36.0f;
    constexpr float k_list_top = 34.0f;

    float header_height(const Hud_Screen& screen) { return screen.show_logo ? 64.0f : 28.0f; }

    float column_width(const Hud_Screen& screen) {
        const int cols = std::max(1, screen.columns);
        return (k_column_width - (cols - 1) * k_gap) / static_cast<float>(cols);
    }

    float extra_above(const Hud_Widget& w) {
        return w.kind == Hud_Widget_Kind::Field ? k_field_caption_h : 0.0f;
    }

    // Размещает виджеты в GUI-координатах: first_row_top — верх первой строки (с учётом подписи поля).
    struct Placed { std::vector<Hud_Rect> rects; float total_height = 0.0f; };

    Placed place_widgets(const Hud_Screen& screen) {
        Placed out;
        const int cols = std::max(1, screen.columns);
        const size_t n = screen.widgets.size();
        out.rects.resize(n);
        const float cw = column_width(screen);
        const float left = -k_column_width * 0.5f; // относительно центра
        float y = 0.0f;
        size_t i = 0;
        while (i < n) {
            const bool full_row = (screen.last_full_width && i + 1 == n) || cols == 1;
            const int in_row = full_row ? 1 : static_cast<int>(std::min<size_t>(cols, n - i - (screen.last_full_width ? 1 : 0)));
            float extra = 0.0f;
            for (int k = 0; k < in_row; ++k) extra = std::max(extra, extra_above(screen.widgets[i + k]));
            y += extra;
            for (int k = 0; k < in_row; ++k) {
                if (full_row) out.rects[i + k] = {left, y, k_column_width, k_button_h};
                else out.rects[i + k] = {left + k * (cw + k_gap), y, cw, k_button_h};
            }
            y += k_button_h + k_gap;
            i += static_cast<size_t>(std::max(1, in_row));
        }
        out.total_height = std::max(0.0f, y - k_gap);
        return out;
    }
}

std::vector<Hud_Rect> Hud::get_widget_rects(int window_width, int window_height, const Hud_Screen& screen) {
    const float s = static_cast<float>(get_gui_scale(window_width, window_height));
    const float gui_w = window_width / s;
    const float gui_h = window_height / s;
    Placed placed = place_widgets(screen);

    float offset_y;
    if (screen.layout == Hud_Screen_Layout::Bottom_Grid) {
        offset_y = gui_h - 8.0f - placed.total_height;
    } else {
        const float header = header_height(screen);
        const float free_h = gui_h - header - placed.total_height;
        offset_y = header + std::max(4.0f, free_h * 0.42f);
        // Не вылезаем за нижний край на маленьких окнах.
        offset_y = std::min(offset_y, gui_h - 6.0f - placed.total_height);
        offset_y = std::max(offset_y, 16.0f);
    }

    std::vector<Hud_Rect> result;
    result.reserve(placed.rects.size());
    for (const Hud_Rect& r : placed.rects) {
        result.push_back({std::floor((gui_w * 0.5f + r.x) * s), std::floor((offset_y + r.y) * s),
                          r.w * s, r.h * s});
    }
    return result;
}

int Hud::get_widget_at(int window_width, int window_height, const Hud_Screen& screen, float mx, float my) {
    const std::vector<Hud_Rect> rects = get_widget_rects(window_width, window_height, screen);
    for (size_t i = 0; i < rects.size(); ++i) {
        if (!screen.widgets[i].enabled) continue;
        if (rects[i].contains(mx, my)) return static_cast<int>(i);
    }
    return -1;
}

float Hud::get_slider_value_at(const Hud_Rect& rect, float mx) {
    // Ручка шириной 8 GUI-пикселей не выходит за края дорожки.
    const float s = rect.h / k_button_h;
    const float knob = 8.0f * s;
    const float usable = std::max(1.0f, rect.w - knob);
    return std::clamp((mx - rect.x - knob * 0.5f) / usable, 0.0f, 1.0f);
}

float Hud::get_world_row_height(int window_width, int window_height) {
    return k_world_row_h * static_cast<float>(get_gui_scale(window_width, window_height));
}

Hud_Rect Hud::get_world_list_rect(int window_width, int window_height, const Hud_Screen& screen) {
    const float s = static_cast<float>(get_gui_scale(window_width, window_height));
    const float gui_w = window_width / s;
    const float gui_h = window_height / s;
    const Placed placed = place_widgets(screen);
    const float bottom = gui_h - 8.0f - placed.total_height - 8.0f;
    const float width = std::min(300.0f, gui_w - 16.0f);
    const float top = k_list_top;
    return {std::floor((gui_w - width) * 0.5f * s), top * s, width * s, std::max(0.0f, (bottom - top)) * s};
}

float Hud::get_world_list_max_scroll(int window_width, int window_height, const Hud_Screen& screen, size_t entry_count) {
    const Hud_Rect list = get_world_list_rect(window_width, window_height, screen);
    const float content = static_cast<float>(entry_count) * get_world_row_height(window_width, window_height);
    return std::max(0.0f, content - list.h);
}

int Hud::get_world_row_at(int window_width, int window_height, const Hud_Screen& screen,
                          size_t entry_count, float scroll_px, float mx, float my) {
    const Hud_Rect list = get_world_list_rect(window_width, window_height, screen);
    if (!list.contains(mx, my)) return -1;
    const float row_h = get_world_row_height(window_width, window_height);
    const int index = static_cast<int>(std::floor((my - list.y + scroll_px) / row_h));
    if (index < 0 || static_cast<size_t>(index) >= entry_count) return -1;
    return index;
}

// ---------------------------------------------------------------------------
//  Отрисовка
// ---------------------------------------------------------------------------
void Hud::draw_widget(const Hud_Widget& widget, const Hud_Rect& rect, bool hovered,
                      float scale, bool caret_visible) {
    const glm::vec4 white(1.0f);
    const glm::vec4 hover_color(1.0f, 1.0f, 0.65f, 1.0f);
    const glm::vec4 off_color(0.5f, 0.5f, 0.52f, 1.0f);
    const float text_y = rect.y + 4.0f * scale;

    switch (widget.kind) {
    case Hud_Widget_Kind::Button: {
        const char* sprite_name = !widget.enabled ? "btn_off" : (hovered ? "btn_hover" : "btn");
        draw_nine_slice(sprite_name, rect.x, rect.y, rect.w, rect.h, 3.0f, scale);
        draw_text_centered(widget.label, rect.x + rect.w * 0.5f, text_y, scale,
                           !widget.enabled ? off_color : (hovered ? hover_color : white));
        break;
    }
    case Hud_Widget_Kind::Slider: {
        draw_nine_slice("slider_track", rect.x, rect.y, rect.w, rect.h, 3.0f, scale);
        const float knob_w = 8.0f * scale;
        const float kx = rect.x + std::clamp(widget.value, 0.0f, 1.0f) * (rect.w - knob_w);
        draw_nine_slice(hovered ? "knob_hover" : "knob", kx, rect.y, knob_w, rect.h, 3.0f, scale);
        std::string text = widget.label;
        if (!widget.value_text.empty()) text += ": " + widget.value_text;
        draw_text_centered(text, rect.x + rect.w * 0.5f, text_y, scale, hovered ? hover_color : white);
        break;
    }
    case Hud_Widget_Kind::Field: {
        if (!widget.label.empty()) {
            draw_text(widget.label, rect.x, rect.y - 11.0f * scale, scale, glm::vec4(0.75f, 0.77f, 0.8f, 1.0f));
        }
        draw_nine_slice(widget.focused ? "field_focus" : "field", rect.x, rect.y, rect.w, rect.h, 2.0f, scale);
        const float pad = 5.0f * scale;
        const float max_w = rect.w - 2.0f * pad;
        std::string shown = widget.text;
        while (shown.size() > 1 && get_text_width(shown, scale) > max_w - scale * 2.0f) shown.erase(0, 1);
        if (widget.text.empty() && !widget.placeholder.empty()) {
            draw_text(widget.placeholder, rect.x + pad, text_y, scale, glm::vec4(0.45f, 0.45f, 0.48f, 1.0f), false);
        } else {
            draw_text(shown, rect.x + pad, text_y, scale);
        }
        if (widget.focused && caret_visible) {
            const float cx = rect.x + pad + (shown.empty() ? 0.0f : get_text_width(shown, scale) + scale);
            draw_rect(cx, rect.y + 5.0f * scale, scale, 10.0f * scale, glm::vec4(1.0f));
        }
        break;
    }
    }
}

void Hud::draw_menu_screen(int window_width, int window_height, const Hud_Screen& screen,
                           int hovered_widget,
                           const std::vector<Hud_World_Entry>* worlds,
                           int selected_world, float scroll_px, bool caret_visible) {
    begin_ui(window_width, window_height);
    const float s = static_cast<float>(get_gui_scale(window_width, window_height));
    const float W = static_cast<float>(window_width);
    const float H = static_cast<float>(window_height);

    if (screen.dirt_background) {
        draw_tiled_background(s);
    } else {
        draw_gradient(0, 0, W, H, glm::vec4(0.02f, 0.02f, 0.04f, 0.62f), glm::vec4(0.02f, 0.02f, 0.04f, 0.78f));
    }

    // Заголовок или логотип.
    if (screen.show_logo) {
        const Sprite& logo = sprite("logo");
        const float lw = logo.rect.z * s;
        const float lh = logo.rect.w * s;
        const float lx = std::floor((W - lw) * 0.5f);
        const float ly = std::floor(10.0f * s);
        draw_sprite("logo", lx + s, ly + s, lw, lh, glm::vec4(0, 0, 0, 0.45f)); // тень логотипа
        draw_sprite("logo", lx, ly, lw, lh);
        if (!screen.subtitle.empty()) {
            draw_text_centered(screen.subtitle, W * 0.5f, ly + lh + 3.0f * s, s, glm::vec4(1.0f, 0.95f, 0.45f, 1.0f));
        }
    } else {
        draw_text_centered(screen.title, W * 0.5f, 12.0f * s, s * (s >= 2.0f ? 1.0f : 1.0f) + (s >= 2 ? 0.0f : 0.0f),
                           glm::vec4(1.0f));
        if (!screen.subtitle.empty()) {
            draw_text_centered(screen.subtitle, W * 0.5f, 22.0f * s, s, glm::vec4(0.7f, 0.72f, 0.76f, 1.0f));
        }
    }

    // Список миров.
    if (screen.layout == Hud_Screen_Layout::Bottom_Grid && worlds) {
        const Hud_Rect list = get_world_list_rect(window_width, window_height, screen);
        const float row_h = get_world_row_height(window_width, window_height);

        draw_rect(list.x - 2.0f * s, list.y - 2.0f * s, list.w + 4.0f * s, list.h + 4.0f * s, glm::vec4(0, 0, 0, 0.5f));

        flush_ui();
        glEnable(GL_SCISSOR_TEST);
        glScissor(static_cast<GLint>(list.x), static_cast<GLint>(H - (list.y + list.h)),
                  static_cast<GLsizei>(list.w), static_cast<GLsizei>(list.h));

        if (worlds->empty()) {
            draw_text_centered(tr("worlds.empty"), list.x + list.w * 0.5f, list.y + list.h * 0.5f - 4.0f * s,
                               s, glm::vec4(0.7f, 0.72f, 0.76f, 1.0f));
        }
        for (size_t i = 0; i < worlds->size(); ++i) {
            const float ry = list.y + static_cast<float>(i) * row_h - scroll_px;
            if (ry + row_h < list.y || ry > list.y + list.h) continue;
            const Hud_World_Entry& e = (*worlds)[i];
            const bool selected = static_cast<int>(i) == selected_world;
            draw_nine_slice(selected ? "row_sel" : "row", list.x + s, ry + s, list.w - 2.0f * s - 6.0f * s, row_h - 2.0f * s, 2.0f, s);
            draw_sprite("world_icon", list.x + 3.0f * s, ry + 2.0f * s, 32.0f * s, 32.0f * s);
            draw_text(e.name, list.x + 39.0f * s, ry + 4.0f * s, s);
            draw_text(e.detail, list.x + 39.0f * s, ry + 15.0f * s, s, glm::vec4(0.6f, 0.62f, 0.66f, 1.0f));
            draw_text(e.detail2, list.x + 39.0f * s, ry + 25.0f * s, s, glm::vec4(0.6f, 0.62f, 0.66f, 1.0f));
        }

        // Полоса прокрутки.
        const float max_scroll = get_world_list_max_scroll(window_width, window_height, screen, worlds->size());
        if (max_scroll > 0.0f) {
            const float track_x = list.x + list.w - 6.0f * s;
            const float content = static_cast<float>(worlds->size()) * row_h;
            const float knob_h = std::max(16.0f * s, list.h * list.h / content);
            const float knob_y = list.y + (list.h - knob_h) * (scroll_px / max_scroll);
            draw_sprite("scroll_track", track_x, list.y, 6.0f * s, list.h);
            draw_sprite("scroll_knob", track_x, knob_y, 6.0f * s, knob_h);
        }

        flush_ui();
        glDisable(GL_SCISSOR_TEST);

        // Тёмные края списка поверх прокрутки.
        draw_gradient(0, list.y - 6.0f * s, W, 6.0f * s, glm::vec4(0, 0, 0, 0.0f), glm::vec4(0, 0, 0, 0.35f));
    }

    // Виджеты.
    const std::vector<Hud_Rect> rects = get_widget_rects(window_width, window_height, screen);
    for (size_t i = 0; i < screen.widgets.size() && i < rects.size(); ++i) {
        draw_widget(screen.widgets[i], rects[i], static_cast<int>(i) == hovered_widget, s, caret_visible);
    }

    if (!screen.footer.empty()) {
        draw_text(screen.footer, 2.0f * s, H - 10.0f * s, s, glm::vec4(0.6f, 0.62f, 0.66f, 1.0f));
    }
    end_ui();
}

void Hud::draw_loading_screen(int window_width, int window_height, const std::string& title,
                              const std::string& status, float progress) {
    begin_ui(window_width, window_height);
    const float s = static_cast<float>(get_gui_scale(window_width, window_height));
    const float W = static_cast<float>(window_width);
    const float H = static_cast<float>(window_height);

    draw_tiled_background(s);
    draw_text_centered(title, W * 0.5f, H * 0.5f - 24.0f * s, s * (s < 3 ? 1.0f : 1.0f));
    if (!status.empty()) {
        draw_text_centered(status, W * 0.5f, H * 0.5f - 10.0f * s, s, glm::vec4(0.75f, 0.77f, 0.8f, 1.0f));
    }

    const float bw = 150.0f * s;
    const float bh = 8.0f * s;
    const float bx = std::floor((W - bw) * 0.5f);
    const float by = std::floor(H * 0.5f + 4.0f * s);
    draw_nine_slice("field", bx - 2.0f * s, by - 2.0f * s, bw + 4.0f * s, bh + 4.0f * s, 2.0f, s);
    const float fill = bw * std::clamp(progress, 0.0f, 1.0f);
    draw_rect(bx, by, fill, bh, glm::vec4(0.55f, 0.82f, 0.38f, 1.0f));
    draw_rect(bx, by, fill, 2.0f * s, glm::vec4(0.8f, 1.0f, 0.65f, 1.0f));
    draw_rect(bx, by + bh - 2.0f * s, fill, 2.0f * s, glm::vec4(0.32f, 0.55f, 0.2f, 1.0f));
    end_ui();
}
