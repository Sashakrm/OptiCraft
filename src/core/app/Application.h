//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_APPLICATION_H
#define OPTICRAFT_APPLICATION_H

#include <memory>
#include <vector>

class Game;

enum class Game_State_Id {
    Main_Menu,
    World_Select,
    Create_World,
    Loading,
    Playing,
    Settings_Menu,
    Iso_Capture
};

class Application;

class I_Game_State {
public:
    explicit I_Game_State(Application& application) : m_application(application) {}
    virtual ~I_Game_State() = default;

    virtual Game_State_Id get_id() const = 0;
    virtual void on_enter() {}
    virtual void on_exit() {}
    virtual void on_resume() {}
    virtual void handle_input() = 0;
    virtual void update(float delta_time) = 0;
    virtual void render() = 0;

protected:
    Application& m_application;
};

class Application {
public:
    explicit Application(Game& game);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void initialize();
    void handle_input();
    void update(float delta_time);
    void render();

    // Переходы выполняются в конце кадра (apply_pending_transition), чтобы стейт не
    // удалял сам себя посреди собственного handle_input.
    void request_push(Game_State_Id id);          // открыть поверх текущего (настройки, выбор мира...)
    void request_pop();                           // вернуться к предыдущему
    void request_replace_all(Game_State_Id id);   // очистить стек и начать с этого стейта
    void request_start_game() { request_replace_all(Game_State_Id::Playing); }
    void request_open_settings() { request_push(Game_State_Id::Settings_Menu); }
    void request_close_settings() { request_pop(); }
    void request_return_to_main_menu() { request_replace_all(Game_State_Id::Main_Menu); }

    Game& get_game() { return m_game; }
    const Game& get_game() const { return m_game; }
    bool is_playing_below_top() const;

private:
    enum class Pending_Transition { None, Push, Pop, Replace_All };
    Game_State_Id m_pending_id{Game_State_Id::Main_Menu};

    Game& m_game;
    std::vector<std::unique_ptr<I_Game_State>> m_state_stack;
    Pending_Transition m_pending_transition{Pending_Transition::None};

    void apply_pending_transition();
    std::unique_ptr<I_Game_State> make_state(Game_State_Id id);
    I_Game_State* active_state();
    const I_Game_State* active_state() const;

};



#endif //OPTICRAFT_APPLICATION_H
