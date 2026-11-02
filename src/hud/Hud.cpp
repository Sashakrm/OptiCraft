#include "hud/Hud.h"
#include "utils/logging/Logger.h"
#include "utils/text/Utf8.h"
#include "rendering/atlas/Atlas_Registry.h"
#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
    int g_gui_scale_setting = 0;

    void build_box_wireframe(std::vector<float>& out, const glm::vec3& lo, const glm::vec3& hi) {
        const float x = lo.x, y = lo.y, z = lo.z;
        const float corners[8][3] = {
            {x,    y,    z},
            {hi.x, y,    z},
            {hi.x, hi.y, z},
            {x,    hi.y, z},
            {x,    y,    hi.z},
            {hi.x, y,    hi.z},
            {hi.x, hi.y, hi.z},
            {x,    hi.y, hi.z}
        };

        const int edges[12][2] = {
            {0, 1}, {1, 2}, {2, 3}, {3, 0},
            {4, 5}, {5, 6}, {6, 7}, {7, 4},
            {0, 4}, {1, 5}, {2, 6}, {3, 7}
        };

        for (const auto& edge : edges) {
            for (int k = 0; k < 2; ++k) {
                out.push_back(corners[edge[k]][0]);
                out.push_back(corners[edge[k]][1]);
                out.push_back(corners[edge[k]][2]);
            }
        }
    }

    constexpr int k_space_advance = 3;   // ширина пробела в пикселях шрифта
    constexpr int k_glyph_height = 9;    // высота ячейки глифа (7 + выносные элементы)
}

// ---------------------------------------------------------------------------
//  Жизненный цикл
// ---------------------------------------------------------------------------
Hud::Hud() = default;

Hud::~Hud() {
    if (m_highlight_vao) glDeleteVertexArrays(1, &m_highlight_vao);
    if (m_highlight_vbo) glDeleteBuffers(1, &m_highlight_vbo);
    if (m_fullscreen_vao) glDeleteVertexArrays(1, &m_fullscreen_vao);
    if (m_fullscreen_vbo) glDeleteBuffers(1, &m_fullscreen_vbo);
    if (m_ui_vao) glDeleteVertexArrays(1, &m_ui_vao);
    if (m_ui_vbo) glDeleteBuffers(1, &m_ui_vbo);
    delete m_line_shader;
    delete m_vignette_shader;
    delete m_ui_shader;
}

bool Hud::initialize() {
    m_line_shader = new Shader("assets/shaders/line_vertex.glsl", "assets/shaders/line_fragment.glsl");
    m_vignette_shader = new Shader("assets/shaders/vignette_vertex.glsl", "assets/shaders/vignette_fragment.glsl");
    m_ui_shader = new Shader("assets/shaders/ui_vertex.glsl", "assets/shaders/ui_fragment.glsl");

    if (m_line_shader->get_id() == 0 || m_vignette_shader->get_id() == 0 ||
        m_ui_shader->get_id() == 0) {
        LOG_ERROR("Failed to initialize HUD shaders");
        return false;
    }

    m_ui_atlas = Atlas_Registry::get_instance().get_atlas("ui_main");
    if (!m_ui_atlas) {
        LOG_ERROR("Hud: atlas \"ui_main\" is missing (assets/atlases/ui_main.atlas.json)");
        return false;
    }

    // Глифы шрифта: font_<код Unicode> — печатный ASCII, русские буквы и Ё/ё.
    auto load_glyph = [this](char32_t cp) {
        const std::string name = "font_" + std::to_string(static_cast<unsigned int>(cp));
        glm::vec4 rect;
        if (!m_ui_atlas->get_sprite_pixel_rect(name, rect)) return;
        Sprite glyph;
        glyph.uv = m_ui_atlas->get_uv_coords(name);
        glyph.rect = rect;
        m_glyphs.emplace(cp, glyph);
    };
    for (char32_t cp = 33; cp < 127; ++cp) load_glyph(cp);
    for (char32_t cp = 0x410; cp <= 0x44F; ++cp) load_glyph(cp);
    load_glyph(0x401);
    load_glyph(0x451);

    glGenVertexArrays(1, &m_highlight_vao);
    glGenBuffers(1, &m_highlight_vbo);
    glBindVertexArray(m_highlight_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_highlight_vbo);
    glBufferData(GL_ARRAY_BUFFER, 12 * 2 * 3 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    const float fullscreen_quad[] = {
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f
    };
    glGenVertexArrays(1, &m_fullscreen_vao);
    glGenBuffers(1, &m_fullscreen_vbo);
    glBindVertexArray(m_fullscreen_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_fullscreen_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fullscreen_quad), fullscreen_quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Пакет UI: pos(2) + uv(2) + color(4).
    glGenVertexArrays(1, &m_ui_vao);
    glGenBuffers(1, &m_ui_vbo);
    glBindVertexArray(m_ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, 4096 * sizeof(Ui_Vertex), nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Ui_Vertex), nullptr);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Ui_Vertex), reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Ui_Vertex), reinterpret_cast<void*>(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    m_batch.reserve(4096);
    return true;
}

// ---------------------------------------------------------------------------
//  Масштаб
// ---------------------------------------------------------------------------
void Hud::set_gui_scale_setting(int setting) { g_gui_scale_setting = std::clamp(setting, 0, 4); }
int Hud::get_gui_scale_setting() { return g_gui_scale_setting; }

int Hud::get_gui_scale(int window_width, int window_height) {
    const int automatic = std::clamp(std::min(window_width / 320, window_height / 240), 1, 4);
    if (g_gui_scale_setting <= 0) return automatic;
    return std::min(g_gui_scale_setting, std::max(1, std::min(window_width / 320, window_height / 240)));
}

// ---------------------------------------------------------------------------
//  Пакетный рендер UI
// ---------------------------------------------------------------------------
void Hud::begin_ui(int window_width, int window_height) {
    m_window_width = std::max(1, window_width);
    m_window_height = std::max(1, window_height);
    m_batch.clear();
    m_batch_atlas = nullptr;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Hud::end_ui() {
    flush_ui();
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

void Hud::flush_ui() {
    if (m_batch.empty()) return;
    m_ui_shader->use();
    m_ui_shader->set_int("atlas", 0);
    (m_batch_atlas ? m_batch_atlas : m_ui_atlas)->bind(0);
    glBindVertexArray(m_ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_batch.size() * sizeof(Ui_Vertex)),
                 m_batch.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_batch.size()));
    glBindVertexArray(0);
    m_batch.clear();
}

const Hud::Sprite& Hud::sprite(const std::string& name) {
    auto it = m_sprites.find(name);
    if (it != m_sprites.end()) return it->second;
    Sprite s;
    if (m_ui_atlas->get_sprite_pixel_rect(name, s.rect)) {
        s.uv = m_ui_atlas->get_uv_coords(name);
    } else {
        LOG_ERROR("Hud: unknown UI sprite \"" + name + "\"");
    }
    return m_sprites.emplace(name, s).first->second;
}

void Hud::quad(float x, float y, float w, float h, const glm::vec4& uv, const glm::vec4& color) {
    if (w <= 0.0f || h <= 0.0f) return;
    const float iw = 2.0f / static_cast<float>(m_window_width);
    const float ih = 2.0f / static_cast<float>(m_window_height);
    const float l = x * iw - 1.0f;
    const float r = (x + w) * iw - 1.0f;
    const float t = 1.0f - y * ih;
    const float b = 1.0f - (y + h) * ih;
    // uv = (u0, v_низ, u1, v_верх): верх экранного квада получает v_верх.
    const Ui_Vertex tl{l, t, uv.x, uv.w, color.r, color.g, color.b, color.a};
    const Ui_Vertex tr{r, t, uv.z, uv.w, color.r, color.g, color.b, color.a};
    const Ui_Vertex bl{l, b, uv.x, uv.y, color.r, color.g, color.b, color.a};
    const Ui_Vertex br{r, b, uv.z, uv.y, color.r, color.g, color.b, color.a};
    m_batch.push_back(tl); m_batch.push_back(br); m_batch.push_back(tr);
    m_batch.push_back(tl); m_batch.push_back(bl); m_batch.push_back(br);
    if (m_batch.size() >= 3000) flush_ui();
}

void Hud::draw_sprite(const std::string& name, float x, float y, float w, float h, const glm::vec4& color) {
    quad(x, y, w, h, sprite(name).uv, color);
}

void Hud::draw_sprite_partial(const std::string& name, float x, float y, float w, float h, float fx, float fy) {
    const Sprite& s = sprite(name);
    fx = std::clamp(fx, 0.0f, 1.0f);
    fy = std::clamp(fy, 0.0f, 1.0f);
    if (fx <= 0.0f || fy <= 0.0f) return;
    glm::vec4 uv = s.uv;
    uv.z = uv.x + (uv.z - uv.x) * fx;           // обрезка справа
    const float full_v = uv.w - uv.y;
    uv.w = uv.y + full_v * fy;                   // обрезка сверху (остаётся нижняя часть)
    // Экранный квад: x..x+w*fx, нижняя часть по высоте.
    quad(x, y + h * (1.0f - fy), w * fx, h * fy, uv, glm::vec4(1.0f));
}

void Hud::draw_nine_slice(const std::string& name, float x, float y, float w, float h,
                          float border, float scale, const glm::vec4& color) {
    const Sprite& s = sprite(name);
    const glm::vec2 tex = m_ui_atlas->get_texture_size();
    float bs = border * scale;
    bs = std::min(bs, std::floor(std::min(w, h) * 0.5f));
    const float b = border;

    const float rx = s.rect.x, ry = s.rect.y, sw = s.rect.z, sh = s.rect.w;
    const float us[4] = {rx / tex.x, (rx + b) / tex.x, (rx + sw - b) / tex.x, (rx + sw) / tex.x};
    // v растёт вверх по картинке: верх спрайта — большее v.
    const float vs[4] = {1.0f - ry / tex.y, 1.0f - (ry + b) / tex.y,
                         1.0f - (ry + sh - b) / tex.y, 1.0f - (ry + sh) / tex.y};
    const float xs[4] = {x, x + bs, x + w - bs, x + w};
    const float ys[4] = {y, y + bs, y + h - bs, y + h};

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            quad(xs[col], ys[row], xs[col + 1] - xs[col], ys[row + 1] - ys[row],
                 glm::vec4(us[col], vs[row + 1], us[col + 1], vs[row]), color);
        }
    }
}

void Hud::draw_rect(float x, float y, float width, float height, const glm::vec4& color) {
    const glm::vec4 uv = sprite("white").uv;
    const float cu = (uv.x + uv.z) * 0.5f;
    const float cv = (uv.y + uv.w) * 0.5f;
    quad(x, y, width, height, glm::vec4(cu, cv, cu, cv), color);
}

void Hud::draw_gradient(float x, float y, float w, float h, const glm::vec4& top, const glm::vec4& bottom) {
    const glm::vec4 uv = sprite("white").uv;
    const float cu = (uv.x + uv.z) * 0.5f;
    const float cv = (uv.y + uv.w) * 0.5f;
    const float iw = 2.0f / static_cast<float>(m_window_width);
    const float ih = 2.0f / static_cast<float>(m_window_height);
    const float l = x * iw - 1.0f, r = (x + w) * iw - 1.0f;
    const float t = 1.0f - y * ih, b = 1.0f - (y + h) * ih;
    const Ui_Vertex tl{l, t, cu, cv, top.r, top.g, top.b, top.a};
    const Ui_Vertex tr{r, t, cu, cv, top.r, top.g, top.b, top.a};
    const Ui_Vertex bl{l, b, cu, cv, bottom.r, bottom.g, bottom.b, bottom.a};
    const Ui_Vertex br{r, b, cu, cv, bottom.r, bottom.g, bottom.b, bottom.a};
    m_batch.push_back(tl); m_batch.push_back(br); m_batch.push_back(tr);
    m_batch.push_back(tl); m_batch.push_back(bl); m_batch.push_back(br);
}

float Hud::get_text_width(const std::string& text, float scale) const {
    if (text.empty()) return 0.0f;
    float width = 0.0f;
    for (size_t i = 0; i < text.size();) {
        const char32_t cp = Utf8::next(text, i);
        if (cp == ' ') {
            width += (k_space_advance + 1) * scale;
        } else if (const auto it = m_glyphs.find(cp); it != m_glyphs.end()) {
            width += (it->second.rect.z + 1.0f) * scale;
        } else {
            width += 4.0f * scale;
        }
    }
    return width - scale;
}

void Hud::draw_text(const std::string& text, float x, float y, float scale,
                    const glm::vec4& color, bool shadow) {
    if (text.empty()) return;
    scale = std::max(1.0f, std::floor(scale));
    x = std::floor(x);
    y = std::floor(y);
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
        const glm::vec4 c = pass == 0 ? glm::vec4(color.r * 0.22f, color.g * 0.22f, color.b * 0.22f, color.a)
                                      : color;
        const float off = pass == 0 ? scale : 0.0f;
        float cx = x + off;
        for (size_t i = 0; i < text.size();) {
            const char32_t cp = Utf8::next(text, i);
            if (cp == ' ') { cx += (k_space_advance + 1) * scale; continue; }
            const auto it = m_glyphs.find(cp);
            if (it == m_glyphs.end()) { cx += 4.0f * scale; continue; }
            const Sprite& g = it->second;
            quad(cx, y + off, g.rect.z * scale, k_glyph_height * scale, g.uv, c);
            cx += (g.rect.z + 1.0f) * scale;
        }
    }
}

void Hud::draw_text_centered(const std::string& text, float center_x, float y, float scale,
                             const glm::vec4& color, bool shadow) {
    draw_text(text, center_x - get_text_width(text, scale) * 0.5f, y, scale, color, shadow);
}

void Hud::draw_tiled_background(float scale) {
    const float tile = 16.0f * scale;
    const glm::vec4 tint(0.42f, 0.42f, 0.45f, 1.0f);
    const int cols = static_cast<int>(std::ceil(m_window_width / tile));
    const int rows = static_cast<int>(std::ceil(m_window_height / tile));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            draw_sprite("bg_tile", c * tile, r * tile, tile, tile, tint);
        }
    }
    // Лёгкое затемнение к краям, чтобы центр экрана читался лучше.
    const float band = 40.0f * scale;
    draw_gradient(0, 0, static_cast<float>(m_window_width), band, glm::vec4(0, 0, 0, 0.55f), glm::vec4(0, 0, 0, 0.0f));
    draw_gradient(0, m_window_height - band, static_cast<float>(m_window_width), band,
                  glm::vec4(0, 0, 0, 0.0f), glm::vec4(0, 0, 0, 0.55f));
}

void Hud::quad_skewed(const glm::vec2& tl, const glm::vec2& tr, const glm::vec2& br, const glm::vec2& bl,
                      const glm::vec4& uv, const glm::vec4& color) {
    const float iw = 2.0f / static_cast<float>(m_window_width);
    const float ih = 2.0f / static_cast<float>(m_window_height);
    auto vertex = [&](const glm::vec2& p, float u, float v) {
        return Ui_Vertex{p.x * iw - 1.0f, 1.0f - p.y * ih, u, v, color.r, color.g, color.b, color.a};
    };
    // Те же соглашения об UV, что у quad(): верх квада получает uv.w, низ — uv.y.
    const Ui_Vertex vtl = vertex(tl, uv.x, uv.w);
    const Ui_Vertex vtr = vertex(tr, uv.z, uv.w);
    const Ui_Vertex vbr = vertex(br, uv.z, uv.y);
    const Ui_Vertex vbl = vertex(bl, uv.x, uv.y);
    m_batch.push_back(vtl); m_batch.push_back(vbr); m_batch.push_back(vtr);
    m_batch.push_back(vtl); m_batch.push_back(vbl); m_batch.push_back(vbr);
    if (m_batch.size() >= 3000) flush_ui();
}

void Hud::draw_slot_icon(const Hud_Inventory_Slot& slot, float x, float y, float size) {
    if (!slot.as_cube) {
        quad(x, y, size, size, slot.icon_uv, glm::vec4(1.0f));
        return;
    }
    // Куб в квадрате size x size, вид сверху-сбоку (изометрия 2:1, как иконки блоков в Minecraft):
    //          T
    //      L       R        T, R, C, L — верхняя грань
    //          C            L, C, B, LL — левая грань
    //      LL      RL       C, R, RL, B — правая грань
    //          B
    const float cx = x + size * 0.5f;
    const glm::vec2 T{cx, y}, R{x + size, y + size * 0.25f}, C{cx, y + size * 0.5f},
                    L{x, y + size * 0.25f}, RL{x + size, y + size * 0.75f},
                    B{cx, y + size}, LL{x, y + size * 0.75f};
    constexpr float k_shade_top = 1.0f, k_shade_left = 0.82f, k_shade_right = 0.62f;
    quad_skewed(T, R, C, L, slot.uv_cube_top, glm::vec4(k_shade_top, k_shade_top, k_shade_top, 1.0f));
    quad_skewed(L, C, B, LL, slot.uv_cube_left, glm::vec4(k_shade_left, k_shade_left, k_shade_left, 1.0f));
    quad_skewed(C, R, RL, B, slot.uv_cube_right, glm::vec4(k_shade_right, k_shade_right, k_shade_right, 1.0f));
}

// Иконки блоков лежат в другой текстуре, чем UI. Всё накопленное из UI-атласа сбрасываем,
// после чего все иконки идут одним пакетом с привязанным атласом блоков.
void Hud::begin_icon_batch(const Texture_Atlas& atlas) {
    flush_ui();
    m_batch_atlas = &atlas;
}

void Hud::end_icon_batch() {
    flush_ui();
    m_batch_atlas = nullptr;
}

void Hud::draw_slot_contents(const std::vector<Hud_Inventory_Slot>& slots,
                             const std::vector<glm::vec2>& icon_positions,
                             float icon_size, float scale, const Texture_Atlas& atlas) {
    const size_t n = std::min(slots.size(), icon_positions.size());
    begin_icon_batch(atlas);
    for (size_t i = 0; i < n; ++i) {
        if (slots[i].empty) continue;
        draw_slot_icon(slots[i], icon_positions[i].x, icon_positions[i].y, icon_size);
    }
    end_icon_batch();
    // Полоска прочности под иконкой повреждённого инструмента (зелёная -> красная).
    for (size_t i = 0; i < n; ++i) {
        if (slots[i].empty || slots[i].durability < 0.0f) continue;
        const float f = std::clamp(slots[i].durability, 0.0f, 1.0f);
        const float bar_w = 13.0f * scale;
        const float x = icon_positions[i].x + 1.5f * scale;
        const float y = icon_positions[i].y + 13.0f * scale;
        draw_rect(x, y, bar_w, 2.0f * scale, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        draw_rect(x, y, std::max(scale, bar_w * f), scale,
                  glm::vec4(1.0f - f, std::min(1.0f, f * 1.6f), 0.1f, 1.0f));
    }
    for (size_t i = 0; i < n; ++i) {
        if (slots[i].empty || slots[i].count <= 1) continue;
        const std::string text = std::to_string(slots[i].count);
        const float tw = get_text_width(text, scale);
        draw_text(text, icon_positions[i].x + icon_size + scale - tw,
                  icon_positions[i].y + icon_size - 7.0f * scale + scale, scale);
    }
}

// ---------------------------------------------------------------------------
//  Игровой HUD
// ---------------------------------------------------------------------------
void Hud::draw_crosshair(int window_width, int window_height, bool target_hit) {
    begin_ui(window_width, window_height);
    const float s = static_cast<float>(get_gui_scale(window_width, window_height));
    const float size = 15.0f * s;
    // Инверсия цвета фона — прицел виден и на светлом небе, и в тёмной пещере.
    glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ZERO);
    draw_sprite("crosshair", std::floor((window_width - size) * 0.5f), std::floor((window_height - size) * 0.5f),
                size, size, target_hit ? glm::vec4(1.0f, 0.55f, 0.55f, 1.0f) : glm::vec4(1.0f));
    flush_ui();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    end_ui();
}

void Hud::draw_player_status(int window_width, int window_height,
                             int health, int max_health, int food, int max_food,
                             float air, float max_air) {
    begin_ui(window_width, window_height);
    const float s = static_cast<float>(get_gui_scale(window_width, window_height));
    const double now = glfwGetTime();

    if (m_last_health >= 0 && health < m_last_health) m_heart_flash_until = now + 0.7;
    m_last_health = health;
    const bool flashing = now < m_heart_flash_until && (static_cast<int>(now * 8.0) % 2 == 0);

    const float hotbar_x = std::floor((window_width - 182.0f * s) * 0.5f);
    const float hotbar_y = window_height - 22.0f * s;
    const float row_y = hotbar_y - 10.0f * s;

    const int heart_units = 10;
    const int hp_per_heart = std::max(1, max_health / heart_units);
    const bool low = health * 4 <= max_health; // <= 25%: сердца дрожат
    for (int i = 0; i < heart_units; ++i) {
        float jitter = 0.0f;
        if (low) jitter = (std::sin(now * 22.0 + i * 1.7) > 0.4) ? s : 0.0f;
        const float x = hotbar_x + i * 8.0f * s;
        const float y = row_y - jitter;
        draw_sprite(flashing ? "heart_flash" : "heart_bg", x, y, 9.0f * s, 9.0f * s);
        const int lo = i * hp_per_heart;
        if (health >= lo + hp_per_heart) {
            draw_sprite("heart_full", x, y, 9.0f * s, 9.0f * s);
        } else if (health > lo + hp_per_heart / 2 - (hp_per_heart % 2 == 0 ? 0 : 0) && health > lo) {
            draw_sprite("heart_half", x, y, 9.0f * s, 9.0f * s);
        }
    }

    // Пузырьки воздуха над сердцами: показываем, только когда голова под водой и воздух не полный.
    if (max_air > 0.0f && air < max_air) {
        const float bubbles_y = row_y - 10.0f * s;
        const float units = max_air / static_cast<float>(heart_units);
        for (int i = 0; i < heart_units; ++i) {
            const float x = hotbar_x + 182.0f * s - 9.0f * s - i * 8.0f * s;
            draw_rect(x, bubbles_y, 9.0f * s, 9.0f * s, glm::vec4(0.05f, 0.12f, 0.25f, 0.85f));
            if (air >= (i + 1) * units) {
                draw_rect(x + 2.0f * s, bubbles_y + 2.0f * s, 5.0f * s, 5.0f * s, glm::vec4(0.55f, 0.82f, 1.0f, 1.0f));
            } else if (air > i * units) {
                draw_rect(x + 2.0f * s, bubbles_y + 2.0f * s, 5.0f * s * (air - i * units) / units,
                          5.0f * s, glm::vec4(0.55f, 0.82f, 1.0f, 1.0f));
            }
        }
    }

    const int food_per_unit = std::max(1, max_food / heart_units);
    for (int i = 0; i < heart_units; ++i) {
        const float x = hotbar_x + 182.0f * s - 9.0f * s - i * 8.0f * s;
        float shake = 0.0f;
        if (food <= 0) shake = (std::sin(now * 25.0 + i * 2.3) > 0.5) ? s : 0.0f;
        const float y = row_y + shake;
        draw_sprite("food_bg", x, y, 9.0f * s, 9.0f * s);
        const int lo = i * food_per_unit;
        if (food >= lo + food_per_unit) {
            draw_sprite("food_full", x, y, 9.0f * s, 9.0f * s);
        } else if (food > lo) {
            draw_sprite("food_half", x, y, 9.0f * s, 9.0f * s);
        }
    }
    end_ui();
}

void Hud::draw_action_progress(int window_width, int window_height, float fraction, const glm::vec4& color) {
    begin_ui(window_width, window_height);
    const float s = static_cast<float>(get_gui_scale(window_width, window_height));
    const float width = 40.0f * s;
    const float height = 3.0f * s;
    const float x = std::floor((window_width - width) * 0.5f);
    const float y = std::floor(window_height * 0.5f + 10.0f * s);
    draw_rect(x - s, y - s, width + 2.0f * s, height + 2.0f * s, glm::vec4(0.0f, 0.0f, 0.0f, 0.7f));
    draw_rect(x, y, width * std::clamp(fraction, 0.0f, 1.0f), height, color);
    end_ui();
}

void Hud::draw_hotbar(int window_width, int window_height,
                      const Texture_Atlas& block_atlas,
                      const std::vector<Hud_Inventory_Slot>& slots,
                      int selected_index, bool survival) {
    const size_t visible = std::min<size_t>(slots.size(), 9);
    if (visible == 0) return;

    begin_ui(window_width, window_height);
    const float s = static_cast<float>(get_gui_scale(window_width, window_height));
    const float hx = std::floor((window_width - 182.0f * s) * 0.5f);
    const float hy = window_height - 22.0f * s;

    draw_sprite("hotbar", hx, hy, 182.0f * s, 22.0f * s);
    if (selected_index >= 0 && static_cast<size_t>(selected_index) < visible) {
        draw_sprite("hotbar_sel", hx - s + selected_index * 20.0f * s, hy - s, 24.0f * s, 24.0f * s);
    }

    std::vector<glm::vec2> positions;
    positions.reserve(visible);
    for (size_t i = 0; i < visible; ++i) {
        positions.emplace_back(hx + (3.0f + 20.0f * static_cast<float>(i)) * s, hy + 3.0f * s);
    }
    std::vector<Hud_Inventory_Slot> shown(slots.begin(), slots.begin() + static_cast<long>(visible));
    draw_slot_contents(shown, positions, 16.0f * s, s, block_atlas);

    // Название выбранного предмета всплывает над хотбаром на пару секунд после смены слота.
    const double now = glfwGetTime();
    if (selected_index != m_last_selected_slot) {
        m_last_selected_slot = selected_index;
        m_selected_name_until = now + 2.0;
    }
    if (selected_index >= 0 && static_cast<size_t>(selected_index) < slots.size() &&
        !slots[static_cast<size_t>(selected_index)].name.empty() && now < m_selected_name_until) {
        const float alpha = static_cast<float>(std::clamp(m_selected_name_until - now, 0.0, 0.5) / 0.5);
        const std::string& name = slots[static_cast<size_t>(selected_index)].name;
        const float ny = hy - (survival ? 28.0f : 18.0f) * s;
        draw_text_centered(name, window_width * 0.5f, ny, s, glm::vec4(1.0f, 1.0f, 1.0f, alpha));
    }
    end_ui();
}

void Hud::draw_vignette(int window_width, int window_height) {
    if (window_width <= 0 || window_height <= 0) return;
    m_window_width = window_width;
    m_window_height = window_height;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_vignette_shader->use();
    m_vignette_shader->set_float("aspect", static_cast<float>(window_width) / static_cast<float>(window_height));
    m_vignette_shader->set_float("radius", 0.45f);
    m_vignette_shader->set_float("softness", 0.55f);
    m_vignette_shader->set_float("strength", 0.3f);

    glBindVertexArray(m_fullscreen_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

void Hud::draw_underwater_overlay(int window_width, int window_height) {
    if (window_width <= 0 || window_height <= 0) return;
    begin_ui(window_width, window_height);
    draw_rect(0.0f, 0.0f, static_cast<float>(window_width), static_cast<float>(window_height),
              glm::vec4(0.06f, 0.24f, 0.48f, 0.45f));
    end_ui();
}

void Hud::draw_block_highlight(const glm::ivec3& block_pos,
                               const glm::mat4& view,
                               const glm::mat4& projection) {
    const glm::vec3 lo(static_cast<float>(block_pos.x), static_cast<float>(block_pos.y),
                       static_cast<float>(block_pos.z));
    draw_box_highlight(lo, lo + glm::vec3(1.0f), view, projection);
}

void Hud::draw_box_highlight(const glm::vec3& lo, const glm::vec3& hi,
                             const glm::mat4& view,
                             const glm::mat4& projection) {
    std::vector<float> vertices;
    build_box_wireframe(vertices, lo - glm::vec3(0.002f), hi + glm::vec3(0.002f));

    glLineWidth(2.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    m_line_shader->use();
    m_line_shader->set_mat4("model", glm::mat4(1.0f));
    m_line_shader->set_mat4("view", view);
    m_line_shader->set_mat4("projection", projection);
    m_line_shader->set_vec3("color", glm::vec3(0.0f, 0.0f, 0.0f));

    glBindVertexArray(m_highlight_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_highlight_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(float), vertices.data());
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(vertices.size() / 3));
    glBindVertexArray(0);

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
}

void Hud::draw_debug_overlay(int window_width, int window_height, const Hud_Debug_Stats& stats) {
    begin_ui(window_width, window_height);
    const float s = static_cast<float>(std::max(1, get_gui_scale(window_width, window_height) - 1));

    char buffer[128];
    std::vector<std::string> lines;
    lines.emplace_back("OptiCraft (Classic voxel engine)");
    std::snprintf(buffer, sizeof(buffer), "%.0f fps, %.2f ms", stats.fps, stats.frame_time_ms);
    lines.emplace_back(buffer);
    std::snprintf(buffer, sizeof(buffer), "Seed: %u  Gen: %s", stats.seed, stats.generation_folder.c_str());
    lines.emplace_back(buffer);
    std::snprintf(buffer, sizeof(buffer), "Chunks: %d / %d  Tris: %d",
                  static_cast<int>(stats.chunks_rendered), static_cast<int>(stats.chunks_loaded),
                  static_cast<int>(stats.triangles_rendered));
    lines.emplace_back(buffer);
    std::snprintf(buffer, sizeof(buffer), "Sections: %d / %d",
                  static_cast<int>(stats.sections_rendered),
                  static_cast<int>(stats.sections_rendered + stats.sections_culled));
    lines.emplace_back(buffer);
    lines.emplace_back("");
    std::snprintf(buffer, sizeof(buffer), "XYZ: %.3f / %.3f / %.3f",
                  stats.player_position.x, stats.player_position.y, stats.player_position.z);
    lines.emplace_back(buffer);
    lines.emplace_back(stats.is_flying ? "State: ghost (flying)" : "State: walker");
    lines.emplace_back("Biome: " + stats.biome_name);
    if (!stats.effects_line.empty()) lines.emplace_back(stats.effects_line);

    const float hours_float = std::fmod(stats.day_night_normalized_time * 24.0f + 6.0f, 24.0f);
    const int hh = static_cast<int>(hours_float);
    const int mm = static_cast<int>((hours_float - static_cast<float>(hh)) * 60.0f);
    std::snprintf(buffer, sizeof(buffer), "Time: %02d:%02d", hh, mm);
    lines.emplace_back(buffer);

    if (stats.extended) {
        lines.emplace_back("");
        std::snprintf(buffer, sizeof(buffer), "Frame (last s): min %.2f / avg %.2f / max %.2f ms",
                      stats.frame_min_ms, stats.frame_avg_ms, stats.frame_max_ms);
        lines.emplace_back(buffer);
        std::snprintf(buffer, sizeof(buffer), "TPS: %.1f (target 20)", stats.tps);
        lines.emplace_back(buffer);
        std::snprintf(buffer, sizeof(buffer), "Mobs alive: hostile %d / passive %d",
                      stats.hostile_count, stats.passive_count);
        lines.emplace_back(buffer);
        std::snprintf(buffer, sizeof(buffer), "Chunks updating: mesh %d (requeued %d), loading %d",
                      static_cast<int>(stats.chunk_mesh_pending), static_cast<int>(stats.chunk_mesh_requeued),
                      static_cast<int>(stats.chunk_loads_requested));
        lines.emplace_back(buffer);
        lines.emplace_back("");
        for (const auto& target_line : stats.target_lines) {
            lines.push_back(target_line);
        }
    }

    const float line_h = 10.0f * s;
    float y = 2.0f * s;
    for (const auto& line : lines) {
        if (!line.empty()) {
            const float w = get_text_width(line, s);
            draw_rect(1.0f * s, y - s, w + 3.0f * s, line_h, glm::vec4(0.0f, 0.0f, 0.0f, 0.45f));
            draw_text(line, 2.0f * s, y, s, glm::vec4(0.88f, 0.88f, 0.88f, 1.0f));
        }
        y += line_h;
    }
    end_ui();
}

void Hud::draw_chat_overlay(int window_width, int window_height,
                            const std::vector<Hud_Chat_Line>& log_lines,
                            const std::string& input_line,
                            bool chat_open,
                            bool caret_visible) {
    const size_t max_lines = chat_open ? 14 : 8;
    std::vector<const Hud_Chat_Line*> visible;
    for (auto it = log_lines.rbegin(); it != log_lines.rend() && visible.size() < max_lines; ++it) {
        if (chat_open || it->time_left > 0.0f) visible.push_back(&*it);
    }
    if (visible.empty() && !chat_open) return;

    begin_ui(window_width, window_height);
    const float gs = static_cast<float>(get_gui_scale(window_width, window_height));
    const float s = static_cast<float>(std::max(1, get_gui_scale(window_width, window_height) - 1));
    const float line_h = 10.0f * s;
    const float x = 2.0f * gs;
    const float max_w = std::min(static_cast<float>(window_width) * 0.7f, 320.0f * gs);
    const size_t max_chars = static_cast<size_t>(std::max(10.0f, max_w / (6.0f * s)));

    const float input_h = 12.0f * gs;
    const float input_y = window_height - input_h - 2.0f * gs;
    const float log_bottom = chat_open ? input_y - 4.0f * gs : window_height - 46.0f * gs;

    float y = log_bottom - line_h;
    for (const Hud_Chat_Line* line : visible) {
        std::string text = line->text;
        if (Utf8::length(text) > max_chars) text = Utf8::prefix(text, max_chars - 2) + "..";
        float alpha = 1.0f;
        if (!chat_open) alpha = std::clamp(line->time_left, 0.0f, 1.0f);
        draw_rect(x, y - s, std::max(max_w * 0.5f, get_text_width(text, s) + 4.0f * s), line_h,
                  glm::vec4(0.0f, 0.0f, 0.0f, 0.42f * alpha));
        const glm::vec4 color = line->is_error ? glm::vec4(1.0f, 0.45f, 0.45f, alpha)
                                               : glm::vec4(1.0f, 1.0f, 1.0f, alpha);
        draw_text(text, x + 2.0f * s, y, s, color);
        y -= line_h;
    }

    if (chat_open) {
        draw_rect(2.0f * gs, input_y, window_width - 4.0f * gs, input_h, glm::vec4(0.0f, 0.0f, 0.0f, 0.55f));
        std::string shown = input_line;
        const size_t input_max = static_cast<size_t>(std::max(10.0f, (window_width - 24.0f * gs) / (6.0f * gs)));
        shown = Utf8::suffix(shown, input_max);
        const float ty = input_y + (input_h - 7.0f * gs) * 0.5f;
        draw_text(shown, 5.0f * gs, ty, gs);
        if (caret_visible) {
            const float cx = 5.0f * gs + (shown.empty() ? 0.0f : get_text_width(shown, gs) + gs);
            draw_rect(cx, ty, gs, 8.0f * gs, glm::vec4(1.0f));
        }
    }
    end_ui();
}

// ---------------------------------------------------------------------------
//  Панель слотов: инвентарь, сундук, верстак, печь
// ---------------------------------------------------------------------------
float Hud::get_panel_scale(int window_width, int window_height, const Hud_Panel_Layout& layout) {
    const int gui = get_gui_scale(window_width, window_height);
    const int fit_h = static_cast<int>((window_height - 8) / layout.size.y);
    const int fit_w = static_cast<int>((window_width - 8) / layout.size.x);
    return static_cast<float>(std::max(1, std::min({gui, fit_h, fit_w})));
}

Hud_Rect Hud::get_panel_rect(int window_width, int window_height, const Hud_Panel_Layout& layout) {
    const float s = get_panel_scale(window_width, window_height, layout);
    const float w = layout.size.x * s;
    const float h = layout.size.y * s;
    return {std::floor((window_width - w) * 0.5f), std::floor((window_height - h) * 0.5f), w, h};
}

int Hud::get_panel_slot_at(int window_width, int window_height, const Hud_Panel_Layout& layout, float mx, float my) {
    const Hud_Rect panel = get_panel_rect(window_width, window_height, layout);
    const float s = get_panel_scale(window_width, window_height, layout);
    for (size_t i = 0; i < layout.slots.size(); ++i) {
        const Hud_Panel_Slot& slot = layout.slots[i];
        const float pad = slot.output ? 4.0f : 0.0f;
        const Hud_Rect r{panel.x + (slot.pos.x - pad) * s, panel.y + (slot.pos.y - pad) * s,
                         (18.0f + 2.0f * pad) * s, (18.0f + 2.0f * pad) * s};
        if (r.contains(mx, my)) return static_cast<int>(i);
    }
    return -1;
}

int Hud::get_panel_page_button_at(int window_width, int window_height, const Hud_Panel_Layout& layout, float mx, float my) {
    if (!layout.page_controls) return -1;
    const Hud_Rect panel = get_panel_rect(window_width, window_height, layout);
    const float s = get_panel_scale(window_width, window_height, layout);
    const Hud_Rect prev{panel.x + layout.page_prev_pos.x * s, panel.y + layout.page_prev_pos.y * s, 14.0f * s, 14.0f * s};
    const Hud_Rect next{panel.x + layout.page_next_pos.x * s, panel.y + layout.page_next_pos.y * s, 14.0f * s, 14.0f * s};
    if (prev.contains(mx, my)) return 0;
    if (next.contains(mx, my)) return 1;
    return -1;
}

void Hud::draw_panel_screen(int window_width, int window_height,
                            const Texture_Atlas& block_atlas,
                            const Hud_Panel_Layout& layout,
                            const std::vector<Hud_Inventory_Slot>& slots,
                            int hovered_slot,
                            const Hud_Inventory_Slot* carried_slot,
                            float cursor_x, float cursor_y,
                            const std::string& title) {
    begin_ui(window_width, window_height);
    const float s = get_panel_scale(window_width, window_height, layout);
    const Hud_Rect panel = get_panel_rect(window_width, window_height, layout);

    draw_gradient(0, 0, static_cast<float>(window_width), static_cast<float>(window_height),
                  glm::vec4(0.03f, 0.03f, 0.05f, 0.62f), glm::vec4(0.03f, 0.03f, 0.05f, 0.74f));
    draw_nine_slice("panel", panel.x, panel.y, panel.w, panel.h, 4.0f, s);

    const glm::vec4 label_color(0.25f, 0.25f, 0.25f, 1.0f);
    if (!title.empty()) {
        draw_text(title, panel.x + 8.0f * s, panel.y + 6.0f * s, s, label_color, false);
    }
    for (const Hud_Panel_Label& label : layout.labels) {
        draw_text(label.text, panel.x + label.pos.x * s, panel.y + label.pos.y * s, s, label_color, false);
    }

    if (layout.player_preview) {
        const Hud_Rect& r = layout.preview_rect;
        draw_nine_slice("field", panel.x + r.x * s, panel.y + r.y * s, r.w * s, r.h * s, 2.0f, s,
                        glm::vec4(0.55f, 0.58f, 0.62f, 1.0f));
        // Без 3D-модели — плоский силуэт (фолбэк); с моделью её рисует Game поверх этой рамки.
        if (!layout.preview_model) {
            const float fig = 2.0f * s;
            draw_sprite("player", std::floor(panel.x + (r.x + (r.w - 16.0f * 2.0f) * 0.5f) * s),
                        std::floor(panel.y + (r.y + (r.h - 32.0f * 2.0f) * 0.5f) * s), 16.0f * fig, 32.0f * fig);
        }
    }

    // Фоны слотов и прогресс.
    for (size_t i = 0; i < layout.slots.size(); ++i) {
        const Hud_Panel_Slot& slot = layout.slots[i];
        const float x = panel.x + slot.pos.x * s;
        const float y = panel.y + slot.pos.y * s;
        if (slot.output) {
            draw_nine_slice("slot", x - 4.0f * s, y - 4.0f * s, 26.0f * s, 26.0f * s, 1.0f, s);
        } else {
            draw_sprite("slot", x, y, 18.0f * s, 18.0f * s);
        }
        if (static_cast<int>(i) == hovered_slot) {
            draw_rect(x + s, y + s, 16.0f * s, 16.0f * s, glm::vec4(1.0f, 1.0f, 1.0f, 0.5f));
        }
    }
    for (const Hud_Panel_Progress& p : layout.progress) {
        const float x = panel.x + p.pos.x * s;
        const float y = panel.y + p.pos.y * s;
        if (p.flame) {
            draw_sprite("flame_empty", x, y, 14.0f * s, 14.0f * s);
            draw_sprite_partial("flame_full", x, y, 14.0f * s, 14.0f * s, 1.0f, p.fraction);
        } else {
            draw_sprite("arrow_empty", x, y, 22.0f * s, 15.0f * s);
            draw_sprite_partial("arrow_full", x, y, 22.0f * s, 15.0f * s, p.fraction, 1.0f);
        }
    }

    if (layout.page_controls) {
        const Hud_Rect prev{panel.x + layout.page_prev_pos.x * s, panel.y + layout.page_prev_pos.y * s, 14.0f * s, 14.0f * s};
        const Hud_Rect next{panel.x + layout.page_next_pos.x * s, panel.y + layout.page_next_pos.y * s, 14.0f * s, 14.0f * s};
        const bool hp = prev.contains(cursor_x, cursor_y);
        const bool hn = next.contains(cursor_x, cursor_y);
        draw_nine_slice(hp ? "btn_hover" : "btn", prev.x, prev.y, prev.w, prev.h, 3.0f, s);
        draw_nine_slice(hn ? "btn_hover" : "btn", next.x, next.y, next.w, next.h, 3.0f, s);
        draw_text_centered("<", prev.x + prev.w * 0.5f, prev.y + 3.0f * s, s);
        draw_text_centered(">", next.x + next.w * 0.5f, next.y + 3.0f * s, s);
        draw_text_centered(layout.page_text, panel.x + layout.page_text_pos.x * s,
                           panel.y + layout.page_text_pos.y * s, s, label_color, false);
    }

    // Предметы и количества.
    std::vector<glm::vec2> icon_positions;
    icon_positions.reserve(layout.slots.size());
    for (const Hud_Panel_Slot& slot : layout.slots) {
        icon_positions.emplace_back(panel.x + (slot.pos.x + 1.0f) * s, panel.y + (slot.pos.y + 1.0f) * s);
    }
    draw_slot_contents(slots, icon_positions, 16.0f * s, s, block_atlas);

    // Подсказка под курсором.
    if (hovered_slot >= 0 && static_cast<size_t>(hovered_slot) < slots.size() &&
        !slots[static_cast<size_t>(hovered_slot)].empty && !slots[static_cast<size_t>(hovered_slot)].name.empty() &&
        (!carried_slot || carried_slot->empty)) {
        const std::string& name = slots[static_cast<size_t>(hovered_slot)].name;
        const float tw = get_text_width(name, s);
        const float bw = tw + 8.0f * s;
        const float bh = 14.0f * s;
        float tx = cursor_x + 12.0f * s;
        float ty = cursor_y - 14.0f * s;
        tx = std::min(tx, static_cast<float>(window_width) - bw - 2.0f);
        ty = std::max(ty, 2.0f);
        draw_nine_slice("tooltip", tx, ty, bw, bh, 3.0f, s);
        draw_text(name, tx + 4.0f * s, ty + 3.0f * s, s);
    }

    // Стак «в руке».
    if (carried_slot && !carried_slot->empty) {
        const float size = 16.0f * s;
        const float ix = cursor_x - size * 0.5f;
        const float iy = cursor_y - size * 0.5f;
        begin_icon_batch(block_atlas);
        draw_slot_icon(*carried_slot, ix, iy, size);
        end_icon_batch();
        if (carried_slot->count > 1) {
            const std::string text = std::to_string(carried_slot->count);
            draw_text(text, ix + size + s - get_text_width(text, s), iy + size - 6.0f * s, s);
        }
    }
    end_ui();
}
