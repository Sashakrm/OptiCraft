//
// Created by noktemor on 28.03.2026.
//

#include "core/app/Application.h"
#include <cstdlib>

#include "core/game/Game.h"
#include "hud/Hud.h"
#include "utils/Config.h"
#include "utils/text/Localization.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
#include "world/generation/World_Generator.h"

namespace {
    int hovered_widget(Game& game, const Hud_Screen& screen) {
        const glm::ivec2 size = game.get_window_size();
        const glm::vec2 cursor = game.get_cursor_position();
        return Hud::get_widget_at(size.x, size.y, screen, cursor.x, cursor.y);
    }

    bool left_clicked(Game& game) { return game.is_mouse_button_pressed(GLFW_MOUSE_BUTTON_LEFT); }

    // ------------------------------------------------------------------------
    //  Главное меню
    // ------------------------------------------------------------------------
    class State_Main_Menu final : public I_Game_State {
    public:
        using I_Game_State::I_Game_State;

        Game_State_Id get_id() const override { return Game_State_Id::Main_Menu; }
        void on_enter() override { m_application.get_game().set_gameplay_input_active(false); }
        void on_resume() override { m_application.get_game().set_gameplay_input_active(false); }

        Hud_Screen build() const {
            Hud_Screen screen;
            screen.show_logo = true;
            screen.subtitle = tr("menu.subtitle");
            screen.widgets = {
                Hud_Widget::button(tr("menu.singleplayer")),
                Hud_Widget::button(tr("menu.options")),
                Hud_Widget::button(tr("menu.quit")),
            };
            screen.footer = "OptiCraft";
            return screen;
        }

        void handle_input() override {
            Game& game = m_application.get_game();
            if (!left_clicked(game)) return;
            switch (hovered_widget(game, build())) {
                case 0: m_application.request_push(Game_State_Id::World_Select); break;
                case 1: m_application.request_push(Game_State_Id::Settings_Menu); break;
                case 2: game.request_exit(); break;
                default: break;
            }
        }

        void update(float) override {}

        void render() override {
            Game& game = m_application.get_game();
            const Hud_Screen screen = build();
            game.render_hud_screen(screen, hovered_widget(game, screen));
        }
    };

    // ------------------------------------------------------------------------
    //  Выбор мира
    // ------------------------------------------------------------------------
    class State_World_Select final : public I_Game_State {
    public:
        using I_Game_State::I_Game_State;

        Game_State_Id get_id() const override { return Game_State_Id::World_Select; }

        void on_enter() override { refresh(); }
        void on_resume() override { refresh(); }

        void handle_input() override {
            Game& game = m_application.get_game();
            const glm::ivec2 size = game.get_window_size();
            const glm::vec2 cursor = game.get_cursor_position();
            const Hud_Screen screen = build();

            // Колесо мыши прокручивает список.
            const float max_scroll = Hud::get_world_list_max_scroll(size.x, size.y, screen, m_worlds.size());
            m_scroll = std::clamp(m_scroll - game.get_scroll_delta() * Hud::get_world_row_height(size.x, size.y) * 0.5f,
                                  0.0f, max_scroll);

            if (m_confirm_delete) {
                if (game.is_key_pressed(GLFW_KEY_ESCAPE)) { m_confirm_delete = false; return; }
                if (!left_clicked(game)) return;
                const int w = hovered_widget(game, screen);
                if (w == 0) {
                    if (m_selected >= 0 && static_cast<size_t>(m_selected) < m_worlds.size()) {
                        game.delete_world(m_worlds[static_cast<size_t>(m_selected)].dir);
                    }
                    m_selected = -1;
                    m_confirm_delete = false;
                    refresh();
                } else if (w == 1) {
                    m_confirm_delete = false;
                }
                return;
            }

            if (game.is_key_pressed(GLFW_KEY_ESCAPE)) { m_application.request_pop(); return; }
            if (game.is_key_pressed(GLFW_KEY_DOWN) && !m_worlds.empty()) {
                m_selected = std::min(m_selected + 1, static_cast<int>(m_worlds.size()) - 1);
                scroll_to_selected(size);
            }
            if (game.is_key_pressed(GLFW_KEY_UP) && !m_worlds.empty()) {
                m_selected = std::max(m_selected - 1, 0);
                scroll_to_selected(size);
            }
            if (game.is_key_pressed(GLFW_KEY_ENTER) || game.is_key_pressed(GLFW_KEY_KP_ENTER)) {
                play_selected();
                return;
            }

            if (!left_clicked(game)) return;

            const int row = Hud::get_world_row_at(size.x, size.y, screen, m_worlds.size(), m_scroll, cursor.x, cursor.y);
            if (row >= 0) {
                const double now = glfwGetTime();
                if (row == m_selected && now - m_last_click_time < 0.45) { play_selected(); return; }
                m_selected = row;
                m_last_click_time = now;
                return;
            }

            switch (Hud::get_widget_at(size.x, size.y, screen, cursor.x, cursor.y)) {
                case 0: play_selected(); break;
                case 1: m_application.request_push(Game_State_Id::Create_World); break;
                case 2: if (m_selected >= 0) m_confirm_delete = true; break;
                case 3: m_application.request_pop(); break;
                default: break;
            }
        }

        void update(float) override {}

        void render() override {
            Game& game = m_application.get_game();
            const Hud_Screen screen = build();
            game.render_hud_screen(screen, hovered_widget(game, screen),
                                   m_confirm_delete ? nullptr : &m_entries, m_selected, m_scroll);
        }

    private:
        std::vector<Game::World_Info> m_worlds;
        std::vector<Hud_World_Entry> m_entries;
        int m_selected = -1;
        float m_scroll = 0.0f;
        double m_last_click_time = -10.0;
        bool m_confirm_delete = false;

        void refresh() {
            Game& game = m_application.get_game();
            m_worlds = game.list_worlds();
            m_entries.clear();
            for (const auto& w : m_worlds) m_entries.push_back({w.name, w.detail, w.detail2});
            if (m_selected >= static_cast<int>(m_worlds.size())) m_selected = -1;
            if (m_selected < 0 && !m_worlds.empty()) m_selected = 0;
            m_scroll = 0.0f;
        }

        void scroll_to_selected(const glm::ivec2& size) {
            if (m_selected < 0) return;
            const Hud_Screen screen = build();
            const Hud_Rect list = Hud::get_world_list_rect(size.x, size.y, screen);
            const float row_h = Hud::get_world_row_height(size.x, size.y);
            const float top = m_selected * row_h;
            if (top < m_scroll) m_scroll = top;
            if (top + row_h > m_scroll + list.h) m_scroll = top + row_h - list.h;
        }

        void play_selected() {
            if (m_selected < 0 || static_cast<size_t>(m_selected) >= m_worlds.size()) return;
            Game& game = m_application.get_game();
            if (game.load_world(m_worlds[static_cast<size_t>(m_selected)].dir)) {
                m_application.request_replace_all(Game_State_Id::Loading);
            }
        }

        Hud_Screen build() const {
            Hud_Screen screen;
            screen.layout = Hud_Screen_Layout::Bottom_Grid;
            screen.columns = 2;
            if (m_confirm_delete) {
                screen.layout = Hud_Screen_Layout::Centered;
                screen.columns = 2;
                screen.title = tr("worlds.delete_title");
                if (m_selected >= 0 && static_cast<size_t>(m_selected) < m_worlds.size()) {
                    screen.subtitle = tr("worlds.delete_warning", {m_worlds[static_cast<size_t>(m_selected)].name});
                }
                screen.widgets = {Hud_Widget::button(tr("common.delete")), Hud_Widget::button(tr("common.cancel"))};
                return screen;
            }
            screen.title = tr("worlds.title");
            const bool has = m_selected >= 0 && static_cast<size_t>(m_selected) < m_worlds.size();
            screen.widgets = {
                Hud_Widget::button(tr("worlds.play"), has),
                Hud_Widget::button(tr("worlds.create")),
                Hud_Widget::button(tr("common.delete"), has),
                Hud_Widget::button(tr("common.cancel")),
            };
            return screen;
        }
    };

    // ------------------------------------------------------------------------
    //  Создание мира
    // ------------------------------------------------------------------------
    class State_Create_World final : public I_Game_State {
    public:
        using I_Game_State::I_Game_State;

        Game_State_Id get_id() const override { return Game_State_Id::Create_World; }

        void on_enter() override { focus(0); }
        void on_exit() override { m_application.get_game().end_text_input(); }
        void on_resume() override { focus(m_focus); }

        void handle_input() override {
            Game& game = m_application.get_game();
            if (game.is_key_pressed(GLFW_KEY_ESCAPE)) { m_application.request_pop(); return; }
            if (game.is_key_pressed(GLFW_KEY_TAB)) { focus(m_focus == 0 ? 1 : 0); return; }
            if (game.is_key_pressed(GLFW_KEY_ENTER) || game.is_key_pressed(GLFW_KEY_KP_ENTER)) { create(); return; }
            if (!left_clicked(game)) return;

            const std::vector<Action> actions = layout();
            const int hovered = hovered_widget(game, build());
            if (hovered < 0 || hovered >= static_cast<int>(actions.size())) return;

            switch (actions[static_cast<size_t>(hovered)]) {
                case Action::Name: focus(0); break;
                case Action::Seed: focus(1); break;
                case Action::Mode: m_survival = !m_survival; break;
                case Action::Type: m_type_index = (m_type_index + 1) % k_type_count; break;
                case Action::Biome: cycle_biome(); break;
                case Action::Create: create(); break;
                case Action::Cancel: m_application.request_pop(); break;
            }
        }

        void update(float dt) override { m_application.get_game().update_text_input(dt); }

        void render() override {
            Game& game = m_application.get_game();
            const Hud_Screen screen = build();
            game.render_hud_screen(screen, hovered_widget(game, screen));
        }

    private:
        // Что делает каждая кнопка экрана; порядок совпадает с виджетами в build().
        enum class Action { Name, Seed, Mode, Type, Biome, Create, Cancel };

        // Типы мира в порядке, в котором кнопка «Тип мира» их перебирает. Первое поле — имя
        // измерения (Dimension.cpp), второе — ключ перевода.
        struct World_Type_Choice {
            const char* dimension;
            const char* label_key;
            bool single_biome;     // показывать ли кнопку биома
        };
        static constexpr size_t k_type_count = 6;
        static constexpr std::array<World_Type_Choice, k_type_count> k_types = {{
            {"overworld",      "world_type.normal",        false},
            {"superflat",      "world_type.superflat",     false},
            {"single_biome",   "world_type.single_biome",  true},
            {"single_islands", "world_type.single_islands", true},
            {"flat_test",      "world_type.flat_test",     false},
            {"void_test",      "world_type.void_test",     false},
        }};

        std::string m_name;
        std::string m_seed;
        int m_focus = 0;
        bool m_survival = false;
        size_t m_type_index = 0;
        size_t m_biome_index = 0;

        const World_Type_Choice& current_type() const { return k_types[m_type_index]; }

        // Кнопка биома есть только у миров с одним биомом; остальные кнопки всегда на месте.
        std::vector<Action> layout() const {
            std::vector<Action> actions = {Action::Name, Action::Seed, Action::Mode, Action::Type};
            if (current_type().single_biome) actions.push_back(Action::Biome);
            actions.push_back(Action::Create);
            actions.push_back(Action::Cancel);
            return actions;
        }

        void cycle_biome() {
            const auto& biomes = selectable_single_biomes();
            m_biome_index = (m_biome_index + 1) % biomes.size();
        }

        void focus(int index) {
            m_focus = index;
            Game& game = m_application.get_game();
            if (index == 0) game.begin_text_input(&m_name, 28);
            else game.begin_text_input(&m_seed, 12);
        }

        void create() {
            Game& game = m_application.get_game();
            std::string error;
            const std::string biome = biome_name(selectable_single_biomes()[m_biome_index]);
            if (game.create_world(m_name, m_seed, m_survival ? Game_Mode::Survival : Game_Mode::Creative,
                                  "Classic", current_type().dimension, biome, error)) {
                game.end_text_input();
                m_application.request_replace_all(Game_State_Id::Loading);
            }
        }

        Hud_Screen build() const {
            Hud_Screen screen;
            screen.title = tr("create.title");
            screen.widgets.push_back(Hud_Widget::field(tr("create.world_name"), m_name,
                                                       tr("create.world_name_default"), m_focus == 0));
            screen.widgets.push_back(Hud_Widget::field(tr("create.seed"), m_seed, "", m_focus == 1));
            screen.widgets.push_back(Hud_Widget::button(
                tr("create.game_mode", {tr(m_survival ? "mode.survival" : "mode.creative")})));
            screen.widgets.push_back(Hud_Widget::button(
                tr("create.world_type", {tr(current_type().label_key)})));
            if (current_type().single_biome) {
                screen.widgets.push_back(Hud_Widget::button(
                    tr("create.biome", {biome_name(selectable_single_biomes()[m_biome_index])})));
            }
            screen.widgets.push_back(Hud_Widget::button(tr("worlds.create")));
            screen.widgets.push_back(Hud_Widget::button(tr("common.cancel")));
            return screen;
        }
    };

    // ------------------------------------------------------------------------
    //  Загрузка мира
    // ------------------------------------------------------------------------
    class State_Loading final : public I_Game_State {
    public:
        using I_Game_State::I_Game_State;

        Game_State_Id get_id() const override { return Game_State_Id::Loading; }
        void on_enter() override { m_application.get_game().set_gameplay_input_active(false); }
        void handle_input() override {}

        void update(float dt) override {
            Game& game = m_application.get_game();
            m_time += dt;
            game.pump_world_loading();
            m_progress = std::max(m_progress, game.get_world_load_progress());
            if ((m_progress >= 0.999f && m_time > 0.4f) || m_time > 15.0f) {
                m_application.request_replace_all(Game_State_Id::Playing);
            }
        }

        void render() override {
            Game& game = m_application.get_game();
            game.render_loading_screen(tr("loading.title"), game.get_world_name(), m_progress);
        }

    private:
        float m_time = 0.0f;
        float m_progress = 0.0f;
    };

    // ------------------------------------------------------------------------
    //  Игра (+ пауза, инвентарь, контейнеры, чат)
    // ------------------------------------------------------------------------
    class State_Playing final : public I_Game_State {
    public:
        using I_Game_State::I_Game_State;

        Game_State_Id get_id() const override { return Game_State_Id::Playing; }

        void on_enter() override { m_application.get_game().set_gameplay_input_active(true); }

        void on_exit() override {
            Game& game = m_application.get_game();
            if (m_inventory_open) game.on_inventory_closed();
            if (game.is_container_open()) game.close_container();
            if (game.is_chat_open()) game.close_chat();
            game.set_gameplay_input_active(false);
            game.close_world();
        }

        void on_resume() override {
            Game& game = m_application.get_game();
            game.set_gameplay_input_active(!m_is_paused && !m_inventory_open && !game.is_container_open() && !game.is_chat_open());
        }

        static Hud_Screen pause_screen() {
            Hud_Screen screen;
            screen.title = tr("pause.title");
            screen.dirt_background = false;
            screen.widgets = {
                Hud_Widget::button(tr("pause.back")),
                Hud_Widget::button(tr("menu.options")),
                Hud_Widget::button(tr("pause.save_quit")),
            };
            return screen;
        }

        void handle_input() override {
            Game& game = m_application.get_game();

            // Экран контейнера открывает сам Game (по ПКМ, см. Game::update_gameplay), а не
            // этот стейт, поэтому фиксируем момент открытия здесь: отпускаем курсор один раз.
            const bool container_open = game.is_container_open();
            if (container_open != m_was_container_open) {
                m_was_container_open = container_open;
                game.set_gameplay_input_active(!container_open && !m_is_paused && !m_inventory_open);
            }

            if (container_open) {
                if (game.is_key_pressed(GLFW_KEY_ESCAPE) || game.is_key_pressed(GLFW_KEY_E)) {
                    game.close_container();
                    return;
                }
                if (left_clicked(game)) game.container_click(false);
                else if (game.is_mouse_button_pressed(GLFW_MOUSE_BUTTON_RIGHT)) game.container_click(true);
                return;
            }

            // Чат: пока открыт, забирает весь ввод (в т.ч. Esc — раньше паузы).
            if (game.is_chat_open()) {
                if (game.is_key_pressed(GLFW_KEY_ESCAPE)) { game.close_chat(); return; }
                if (game.is_key_pressed(GLFW_KEY_ENTER) || game.is_key_pressed(GLFW_KEY_KP_ENTER)) {
                    game.submit_chat_line();
                    return;
                }
                if (game.is_key_pressed(GLFW_KEY_TAB))  { game.chat_autocomplete(); return; }
                if (game.is_key_pressed(GLFW_KEY_UP))   { game.chat_history_prev(); return; }
                if (game.is_key_pressed(GLFW_KEY_DOWN)) { game.chat_history_next(); return; }
                return;
            }

            // T или / открывают чат (/ — сразу с префиксом команды).
            if (!m_is_paused && !m_inventory_open) {
                if (game.is_key_pressed(GLFW_KEY_T)) { game.open_chat(false); return; }
                if (game.is_key_pressed(GLFW_KEY_SLASH) || game.is_key_pressed(GLFW_KEY_KP_DIVIDE)) {
                    game.open_chat(true);
                    return;
                }
            }

            if (game.is_key_pressed(GLFW_KEY_ESCAPE)) {
                if (m_inventory_open) {
                    m_inventory_open = false;
                    game.on_inventory_closed();
                    game.set_gameplay_input_active(!m_is_paused);
                    return;
                }
                m_is_paused = !m_is_paused;
                game.set_gameplay_input_active(!m_is_paused);
                return;
            }

            if (!m_is_paused && game.is_key_pressed(GLFW_KEY_E)) {
                m_inventory_open = !m_inventory_open;
                if (!m_inventory_open) game.on_inventory_closed();
                game.set_gameplay_input_active(!m_inventory_open);
                return;
            }

            if (m_inventory_open) {
                if (game.is_key_pressed(GLFW_KEY_RIGHT)) { game.inventory_change_page(1); return; }
                if (game.is_key_pressed(GLFW_KEY_LEFT))  { game.inventory_change_page(-1); return; }
                if (left_clicked(game)) game.inventory_click(false);
                else if (game.is_mouse_button_pressed(GLFW_MOUSE_BUTTON_RIGHT)) game.inventory_click(true);
                return;
            }

            if (!m_is_paused) {
                // F7 — изометрический снимок мира (как в Minecraft Indev).
                if (game.is_key_pressed(GLFW_KEY_F7)) {
                    m_application.request_push(Game_State_Id::Iso_Capture);
                    return;
                }
                game.handle_gameplay_input();
                return;
            }

            if (!left_clicked(game)) return;
            switch (hovered_widget(game, pause_screen())) {
                case 0:
                    m_is_paused = false;
                    game.set_gameplay_input_active(true);
                    break;
                case 1:
                    m_application.request_push(Game_State_Id::Settings_Menu);
                    break;
                case 2:
                    m_application.request_return_to_main_menu();
                    break;
                default:
                    break;
            }
        }

        void update(float delta_time) override {
            Game& game = m_application.get_game();
            // Отладочный хук для автотестов: OPTICRAFT_ISO_AUTOTEST=1 сам запускает изометрический
            // снимок через пару секунд после входа в мир (после него игра закрывается, см.
            // Game::poll_iso_capture_result).
            if (!m_iso_autotest_done && std::getenv("OPTICRAFT_ISO_AUTOTEST")) {
                m_iso_autotest_timer += delta_time;
                if (m_iso_autotest_timer > 3.0f) {
                    m_iso_autotest_done = true;
                    m_application.request_push(Game_State_Id::Iso_Capture);
                }
            }
            game.update_chat(delta_time); // тикает и на паузе: строки лога затухают
            game.update_gameplay(delta_time, !m_is_paused,
                                 !m_inventory_open && !game.is_container_open() && !game.is_chat_open());
        }

        void render() override {
            Game& game = m_application.get_game();
            game.render_gameplay(!m_is_paused && !m_inventory_open && !game.is_container_open() && !game.is_chat_open());
            if (game.is_container_open()) game.draw_container_overlay();
            if (m_inventory_open) game.draw_inventory_overlay();
            if (m_is_paused) {
                const Hud_Screen screen = pause_screen();
                game.render_hud_screen(screen, hovered_widget(game, screen));
            }
        }

    private:
        bool m_is_paused{false};
        bool m_inventory_open{false};
        bool m_was_container_open{false};
        bool m_iso_autotest_done{false};
        float m_iso_autotest_timer{0.0f};
    };

    // ------------------------------------------------------------------------
    //  Изометрический снимок мира (F7): поверх игры, мир стоит, показывается прогресс
    // ------------------------------------------------------------------------
    class State_Iso_Capture final : public I_Game_State {
    public:
        using I_Game_State::I_Game_State;

        Game_State_Id get_id() const override { return Game_State_Id::Iso_Capture; }

        void on_enter() override {
            Game& game = m_application.get_game();
            game.set_gameplay_input_active(false);
            if (!game.start_iso_capture()) m_application.request_pop();
        }

        void handle_input() override {
            Game& game = m_application.get_game();
            if (game.is_key_pressed(GLFW_KEY_ESCAPE)) {
                game.cancel_iso_capture();
                m_application.request_pop();
            }
        }

        void update(float) override {
            Game& game = m_application.get_game();
            game.update_iso_capture();
            // Запись PNG идёт уже в фоне — игру можно продолжать.
            if (!game.is_iso_capture_busy()) m_application.request_pop();
        }

        void render() override {
            m_application.get_game().render_iso_capture_overlay();
        }
    };

    // ------------------------------------------------------------------------
    //  Настройки
    // ------------------------------------------------------------------------
    class State_Settings_Menu final : public I_Game_State {
    public:
        using I_Game_State::I_Game_State;

        Game_State_Id get_id() const override { return Game_State_Id::Settings_Menu; }

        void on_enter() override { m_application.get_game().set_gameplay_input_active(false); }
        void on_exit() override { m_application.get_game().save_settings(); }

        void handle_input() override {
            Game& game = m_application.get_game();
            if (game.is_key_pressed(GLFW_KEY_ESCAPE)) { m_application.request_pop(); return; }

            const Hud_Screen screen = build();
            const glm::ivec2 size = game.get_window_size();
            const glm::vec2 cursor = game.get_cursor_position();

            if (!game.is_mouse_button_held(GLFW_MOUSE_BUTTON_LEFT)) m_drag = -1;

            if (left_clicked(game)) {
                const int w = Hud::get_widget_at(size.x, size.y, screen, cursor.x, cursor.y);
                if (w >= 0) {
                    switch (screen.widgets[static_cast<size_t>(w)].kind) {
                        case Hud_Widget_Kind::Slider: m_drag = w; break;
                        default: click_button(w); break;
                    }
                }
            }

            if (m_drag >= 0) {
                const std::vector<Hud_Rect> rects = Hud::get_widget_rects(size.x, size.y, screen);
                const float t = Hud::get_slider_value_at(rects[static_cast<size_t>(m_drag)], cursor.x);
                apply_slider(m_drag, t);
            }
        }

        void update(float) override {}

        void render() override {
            Game& game = m_application.get_game();
            const Hud_Screen screen = build();
            const int hovered = m_drag >= 0 ? m_drag : hovered_widget(game, screen);
            if (m_application.is_playing_below_top()) game.render_gameplay(false);
            game.render_hud_screen(screen, hovered);
        }

    private:
        int m_drag = -1;

        bool in_game() const { return m_application.is_playing_below_top(); }

        // Индексы виджетов: 0 дальность, 1 трава, 2 чувствительность, 3 масштаб, 4 язык,
        // [5 генерация, 6 режим] — только внутри мира, последний — «Готово».
        static constexpr int k_idx_gui_scale = 3;
        static constexpr int k_idx_language = 4;
        static constexpr int k_idx_generation = 5;
        static constexpr int k_idx_mode = 6;
        int done_index() const { return in_game() ? 7 : 5; }

        Hud_Screen build() const {
            Game& game = m_application.get_game();
            Hud_Screen screen;
            screen.title = tr("options.title");
            screen.columns = 2;
            screen.last_full_width = true;
            screen.dirt_background = !in_game();

            const int rd = game.get_render_distance();
            screen.widgets.push_back(Hud_Widget::slider(tr("options.render_distance"), std::to_string(rd),
                static_cast<float>(rd - Config::render_distance_min) /
                static_cast<float>(Config::render_distance_max - Config::render_distance_min)));
            const int fd = game.get_flora_distance();
            screen.widgets.push_back(Hud_Widget::slider(tr("options.flora_distance"), std::to_string(fd),
                static_cast<float>(fd - 2) / static_cast<float>(Config::flora_render_distance_max - 2)));
            const float sens = game.get_mouse_sensitivity();
            screen.widgets.push_back(Hud_Widget::slider(tr("options.sensitivity"),
                std::to_string(static_cast<int>(std::lround(sens / Config::mouse_sensitivity * 100.0f))) + "%",
                (sens - k_sens_min) / (k_sens_max - k_sens_min)));
            const int gs = game.get_gui_scale_setting();
            screen.widgets.push_back(Hud_Widget::button(
                tr("options.gui_scale", {gs == 0 ? tr("options.gui_scale_auto") : std::to_string(gs) + "x"})));
            screen.widgets.push_back(Hud_Widget::button(
                tr("options.language", {Localization::get().get_language_display_name(Localization::get().get_language())})));
            if (in_game()) {
                screen.widgets.push_back(Hud_Widget::button(tr("options.world", {game.get_generation_folder()})));
                screen.widgets.push_back(Hud_Widget::button(
                    tr("options.mode", {tr(game.get_game_mode() == Game_Mode::Survival ? "mode.survival" : "mode.creative")})));
            }
            screen.widgets.push_back(Hud_Widget::button(tr("common.done")));
            return screen;
        }

        static constexpr float k_sens_min = 0.05f;
        static constexpr float k_sens_max = 0.7f;

        void apply_slider(int index, float t) {
            Game& game = m_application.get_game();
            switch (index) {
                case 0:
                    game.set_render_distance(Config::render_distance_min +
                        static_cast<int>(std::lround(t * (Config::render_distance_max - Config::render_distance_min))));
                    break;
                case 1:
                    game.set_flora_distance(2 + static_cast<int>(std::lround(t * (Config::flora_render_distance_max - 2))));
                    break;
                case 2:
                    game.set_mouse_sensitivity(k_sens_min + t * (k_sens_max - k_sens_min));
                    break;
                default: break;
            }
        }

        void click_button(int index) {
            Game& game = m_application.get_game();
            if (index == k_idx_gui_scale) {
                game.set_gui_scale_setting((game.get_gui_scale_setting() + 1) % 5);
            } else if (index == k_idx_language) {
                game.cycle_language();
            } else if (in_game() && index == k_idx_generation) {
                game.toggle_generation_folder();
            } else if (in_game() && index == k_idx_mode) {
                game.on_inventory_closed();
                game.toggle_game_mode();
            } else if (index == done_index()) {
                m_application.request_pop();
            }
        }
    };
}

Application::Application(Game& game)
    : m_game(game)
{}

Application::~Application() = default;

void Application::initialize() {
    m_state_stack.push_back(make_state(Game_State_Id::Main_Menu));
    m_state_stack.back()->on_enter();

    // Отладочный хук для автотестов: OPTICRAFT_AUTOSTART=survival|creative сразу создаёт мир
    // "AutoTest" (seed 123) и входит в игру, минуя меню.
    if (const char* autostart = std::getenv("OPTICRAFT_AUTOSTART")) {
        std::string error;
        const bool survival = std::string(autostart) == "survival";
        if (m_game.create_world("AutoTest", "123", survival ? Game_Mode::Survival : Game_Mode::Creative,
                                "Classic", "overworld", "Plains", error)) {
            request_replace_all(Game_State_Id::Loading);
        }
    }
}

void Application::handle_input() {
    if (I_Game_State* state = active_state()) {
        state->handle_input();
    }
    apply_pending_transition();
}

void Application::update(float delta_time) {
    if (I_Game_State* state = active_state()) {
        state->update(delta_time);
    }
    apply_pending_transition();
}

void Application::render() {
    if (I_Game_State* state = active_state()) {
        state->render();
    }
}

void Application::request_push(Game_State_Id id) {
    m_pending_transition = Pending_Transition::Push;
    m_pending_id = id;
}

void Application::request_pop() {
    m_pending_transition = Pending_Transition::Pop;
}

void Application::request_replace_all(Game_State_Id id) {
    m_pending_transition = Pending_Transition::Replace_All;
    m_pending_id = id;
}

bool Application::is_playing_below_top() const {
    return m_state_stack.size() >= 2 &&
           m_state_stack[m_state_stack.size() - 2]->get_id() == Game_State_Id::Playing;
}

void Application::apply_pending_transition() {
    const Pending_Transition transition = m_pending_transition;
    m_pending_transition = Pending_Transition::None;

    switch (transition) {
        case Pending_Transition::None:
            return;
        case Pending_Transition::Push:
            m_state_stack.push_back(make_state(m_pending_id));
            m_state_stack.back()->on_enter();
            return;
        case Pending_Transition::Pop:
            if (m_state_stack.size() <= 1) return;
            m_state_stack.back()->on_exit();
            m_state_stack.pop_back();
            m_state_stack.back()->on_resume();
            return;
        case Pending_Transition::Replace_All:
            // Снизу вверх: каждому стейту даём закрыться (Playing сохраняет мир в on_exit).
            while (!m_state_stack.empty()) {
                m_state_stack.back()->on_exit();
                m_state_stack.pop_back();
            }
            m_state_stack.push_back(make_state(m_pending_id));
            m_state_stack.back()->on_enter();
            return;
    }
}

std::unique_ptr<I_Game_State> Application::make_state(Game_State_Id id) {
    switch (id) {
        case Game_State_Id::Main_Menu:
            return std::make_unique<State_Main_Menu>(*this);
        case Game_State_Id::World_Select:
            return std::make_unique<State_World_Select>(*this);
        case Game_State_Id::Create_World:
            return std::make_unique<State_Create_World>(*this);
        case Game_State_Id::Loading:
            return std::make_unique<State_Loading>(*this);
        case Game_State_Id::Playing:
            return std::make_unique<State_Playing>(*this);
        case Game_State_Id::Settings_Menu:
            return std::make_unique<State_Settings_Menu>(*this);
        case Game_State_Id::Iso_Capture:
            return std::make_unique<State_Iso_Capture>(*this);
    }
    return nullptr;
}

I_Game_State* Application::active_state() {
    return m_state_stack.empty() ? nullptr : m_state_stack.back().get();
}

const I_Game_State* Application::active_state() const {
    return m_state_stack.empty() ? nullptr : m_state_stack.back().get();
}
