//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_GAME_H
#define OPTICRAFT_GAME_H

#define GLFW_INCLUDE_NONE

#include "core/app/Window.h"
#include <string>
#include "input/Input_Manager.h"
#include "entities/player/Camera.h"
#include "rendering/Renderer.h"
#include "rendering/capture/Isometric_Capture.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Interaction.h"
#include "world/blocks/Container_Session.h"
#include "entities/player/Player.h"
#include "hud/Hud.h"
#include <memory>
#include <vector>
#include "world/generation/World_Generator.h"
#include "world/storage/World_File.h"
#include "entities/player/Inventory.h"
#include "world/blocks/Block_Interaction.h"
#include "core/game/Day_Night_Cycle.h"
#include "entities/animals/Pig.h"
#include "entities/animals/Horse.h"
#include "entities/animals/Cow.h"
#include "world/crafting/Crop_Manager.h"
#include "entities/items/Dropped_Item.h"

class Application;

class Game {
private:
    std::unique_ptr<Window> m_window_ptr;
    std::unique_ptr<Input_Manager> m_input_ptr;
    std::unique_ptr<Camera> m_camera_ptr;
    std::unique_ptr<Renderer> m_renderer_ptr;
    std::unique_ptr<Chunk_Manager> m_chunk_manager_ptr;
    std::unique_ptr<Player> m_player_ptr;
    std::unique_ptr<Inventory> m_inventory_ptr;
    // Изометрический снимок мира (F7). Объявлен ПОСЛЕ Chunk_Manager и Renderer, поэтому
    // уничтожается раньше них (держит ссылки на оба и, пока пишется PNG, фоновый поток).
    std::unique_ptr<Isometric_Capture> m_iso_capture_ptr;
    // Свиньи в мире — держится после m_window_ptr по объявлению, поэтому
    // уничтожается ДО разрушения GL-контекста (см. предупреждение о времени
    // жизни в src/mobs/Mob_Model.h — иначе деструктор Mob упадёт на мёртвом контексте).
    std::vector<std::unique_ptr<Pig>> m_pigs;
    std::vector<std::unique_ptr<Horse>> m_horses;
    std::vector<std::unique_ptr<Cow>> m_cows;
    // Рост посевов на грядках (см. Crop_Manager.h); сохраняется в crops.txt рядом с миром.
    std::unique_ptr<Crop_Manager> m_crops_ptr;
    // Выпавшие предметы (блоки, мясо...). Не держит GL-ресурсов — рисуется через Renderer,
    // поэтому порядок объявления относительно окна не важен.
    std::unique_ptr<Dropped_Item_Manager> m_dropped_items_ptr;
    std::vector<Chunk_Vertex> m_dropped_items_mesh; // переиспользуемый буфер вершин, чтобы не аллоцировать каждый кадр
    std::unique_ptr<World_Generator> m_world_generator;
    std::unique_ptr<Block_Interaction> m_block_interaction_ptr;
    // Открытый экран контейнера (сундук/верстак/печь) — см. Block_Interaction::
    // has_container_open_request и update_gameplay(). Живёт целиком в Game, а не в
    // Application-стейте: Application лишь опрашивает is_container_open() и не хранит
    // дублирующего состояния (см. комментарий у State_Playing::handle_input).
    Container_Session m_container_session;
    glm::ivec3 m_open_container_pos{};
    Block_Entity* m_open_container_paired = nullptr; // вторая половина двойного сундука, если есть
    std::unique_ptr<Hud> m_hud_ptr;
    std::unique_ptr<Application> m_application_ptr;
    World_File m_world_file;
    World_Settings m_world_settings;
    std::string m_world_dir;            // имя папки текущего мира в worlds/
    bool m_loading_world = false;
    // Приёмник текстового ввода для экранов (поле имени мира и т.п.); чат имеет приоритет.
    std::string* m_text_target = nullptr;
    size_t m_text_max = 32;
    float m_text_backspace_timer = 0.0f;
    int m_gui_scale_setting = 0;        // 0 = авто
    void load_options();
    void save_options() const;
    void setup_world_objects(const glm::vec3* saved_position);
    void teardown_world();
    std::string* active_text_target();

    struct Panel_View {
        Hud_Panel_Layout layout;
        std::vector<Hud_Inventory_Slot> slots;   // параллельно layout.slots
        std::string title;
    };
    Panel_View build_inventory_panel() const;
    Panel_View build_container_panel() const;
    std::string m_generation_folder;
    // Точка изначального спавна игрока (задаётся один раз в initialize()) — сюда игрок
    // возвращается при смерти в Survival (см. update_gameplay).
    glm::vec3 m_spawn_position{8.0f, 0.0f, 8.0f};
    Game_Mode m_game_mode = Game_Mode::Creative;
    // Реальное время (20 минут по умолчанию — см. Config), а не игровые тики: пользователь
    // явно попросил именно длительность в реальных минутах, не привязанную к TPS/FPS.
    Day_Night_Cycle m_day_night_cycle;

    // Текущая дальность прорисовки в чанках. Стартует с Config::chunk_load_radius
    // и меняется из меню настроек.
    int m_render_distance{Config::chunk_load_radius};
    int m_flora_distance{Config::flora_render_distance_chunks};

    float m_fps_update_timer{0.0f};
    int m_fps_frame_count{0};
    float m_current_fps{0.0f};
    bool m_show_debug_hud{false};

    bool m_is_running;
    bool m_is_shutdown{false};
    float m_delta_time;
    float m_last_frame_time;

    // Когда фоновая запись PNG закончилась — сообщает в чат и освобождает снимок.
    void poll_iso_capture_result();
    void log_stats();
    void reload_world(const std::string& generation_folder);
    // Находится ли ТОЧКА ГЛАЗ игрока внутри жидкости. Проверяется именно камера,
    // а не ноги: стоя по пояс в воде экран синим не заливает.
    bool is_camera_underwater() const;
    // Если свиней в мире меньше Config::min_pig_count — досоздаёт недостающих
    // рядом с игроком (случайное смещение по XZ в кольце
    // [pig_spawn_radius_min; pig_spawn_radius_max], Y — как у игрока: террейн
    // под ногами свиньи не сэмплируется, см. предупреждение в Pig.h).
    void spawn_missing_pigs(const glm::vec3& player_position);
    void spawn_missing_cows(const glm::vec3& player_position);
    float m_cow_respawn_cooldown = 0.0f;
    // Размножение животных: ищет влюблённые пары, ведёт их друг к другу и рождает детёнышей.
    void update_breeding();
    // Урожай: выпавшие предметы при сборе ростка и исчезновение дикой грядки.
    void handle_crop_harvest(const glm::ivec3& pos, Block_Types crop_block);

    // --- Лошади -----------------------------------------------------------------
    // Появление — как у свиней (держим Config::min_horse_count рядом с игроком, после убийства
    // пауза Config::horse_respawn_delay), но лошадь ставится на поверхность травы, а не на Y игрока.
    void spawn_missing_horses(const glm::vec3& player_position);
    // Апдейт всех лошадей + посадка/высадка/синхронизация игрока с седлом.
    void update_horses(float delta_time);
    void mount_horse(Horse& horse);
    // place_beside=true — поставить игрока рядом с лошадью (обычная высадка); false — оставить
    // где стоит (его уже телепортировали/респавнили).
    void dismount_horse(bool place_beside);
    Horse* m_ridden_horse = nullptr;      // лошадь, на которой сидит игрок (указатель стабилен: unique_ptr)
    glm::vec3 m_last_seat_position{0.0f}; // куда мы сами поставили игрока в прошлом кадре — чтобы заметить телепорт
    Horse_Ride_Input m_horse_ride_input;  // ввод всадника, собирается в handle_gameplay_input
    bool m_dismount_requested = false;
    float m_horse_respawn_cooldown = 0.0f;

    // Бой: пауза между ударами и задержка респавна свиней после убийства (иначе мясо можно
    // было бы фармить бесконечно — новая свинья появлялась бы в тот же кадр).
    float m_attack_cooldown = 0.0f;
    float m_pig_respawn_cooldown = 0.0f;

    // --- Чат и команды ----------------------------------------------------------
    static constexpr size_t k_chat_log_capacity = 100;   // хранится в логе
    static constexpr size_t k_chat_visible_closed = 8;   // строк видно при закрытом чате
    static constexpr size_t k_chat_visible_open = 14;    // строк видно при открытом
    static constexpr size_t k_chat_input_max = 100;
    static constexpr float k_chat_line_seconds = 10.0f;
    bool m_chat_open{false};
    std::string m_chat_input;
    std::vector<std::string> m_chat_history;   // отправленные строки для стрелок вверх/вниз
    int m_chat_history_index{-1};
    std::string m_chat_history_draft;          // то, что печаталось до входа в историю
    std::vector<Hud_Chat_Line> m_chat_log;
    float m_backspace_timer{0.0f};

public:
    Game(int width, int height, const std::string& title);
    ~Game();

    bool initialize();
    void run();
    void shutdown();

    bool is_key_pressed(int key) const;
    bool is_mouse_button_pressed(int button) const;
    bool is_mouse_button_held(int button) const;

    // --- Миры -------------------------------------------------------------------
    struct World_Info {
        std::string dir;      // имя папки в worlds/
        std::string name;
        std::string detail;   // "name (папка, дата)"
        std::string detail2;  // "Survival, Classic, seed N"
    };
    std::vector<World_Info> list_worlds() const;
    // Создаёт мир в новой папке и сразу загружает. seed_text пустой — случайный сид.
    bool create_world(const std::string& name, const std::string& seed_text, Game_Mode mode,
                      const std::string& generation_folder, std::string& error);
    bool load_world(const std::string& dir);
    void save_world();                 // настройки, позиция игрока, изменённые чанки
    void close_world();                // сохранить и выгрузить (выход в меню)
    bool delete_world(const std::string& dir);
    bool is_world_loaded() const { return m_chunk_manager_ptr != nullptr; }
    // 0..1: сколько чанков вокруг игрока уже готово — для экрана загрузки.
    float get_world_load_progress() const;
    // Пока экран загрузки: принимает готовые чанки и дозапрашивает новые вокруг игрока.
    void pump_world_loading();
    const std::string& get_world_name() const { return m_world_settings.name; }

    // --- Интерфейс --------------------------------------------------------------
    glm::vec2 get_cursor_position() const;
    glm::ivec2 get_window_size() const;
    float get_scroll_delta() const { return m_input_ptr->get_scroll_delta(); }
    bool is_caret_visible() const;
    // Экраны без игры за спиной (главное меню и т.д.) сначала очищают кадр.
    void clear_menu_frame();
    void render_hud_screen(const Hud_Screen& screen, int hovered,
                           const std::vector<Hud_World_Entry>* worlds = nullptr,
                           int selected_world = -1, float scroll_px = 0.0f);
    void render_loading_screen(const std::string& title, const std::string& status, float progress);
    Hud& get_hud() { return *m_hud_ptr; }

    // Текстовый ввод (поля экранов): символы приходят из GLFW, Backspace опрашивается здесь.
    void begin_text_input(std::string* target, size_t max_length);
    void end_text_input();
    void update_text_input(float delta_time);

    // --- Окно инвентаря (клавиша E) и контейнеры -----------------------------------
    // Раскладка строится заново на каждый вызов; позиции для клика и для рисования
    // считает один и тот же код (Hud::get_panel_*), поэтому они не расходятся.
    void draw_inventory_overlay();
    void inventory_click(bool right);
    void inventory_change_page(int direction);
    void on_inventory_closed();
    bool is_container_open() const { return m_container_session.is_open(); }
    void draw_container_overlay();
    void container_click(bool right);
    void close_container();

    // Режим игры (Creative/Survival) — переключается из меню настроек.
    Game_Mode get_game_mode() const { return m_game_mode; }
    std::string get_game_mode_name() const;
    void toggle_game_mode();

    // --- Настройки (меню) ------------------------------------------------------
    int get_render_distance() const { return m_render_distance; }
    void set_render_distance(int chunks);
    int get_flora_distance() const { return m_flora_distance; }
    void set_flora_distance(int chunks);
    float get_mouse_sensitivity() const { return m_input_ptr->get_mouse_sensitivity(); }
    void set_mouse_sensitivity(float value);
    int get_gui_scale_setting() const { return m_gui_scale_setting; }
    void set_gui_scale_setting(int value);
    void cycle_language();   // следующий язык из assets/lang, сохраняется в options.txt
    void save_settings() const { save_options(); }

    // --- Изометрический снимок мира (клавиша F7) ----------------------------------------------
    // start — начинает снимок (false, если мира нет или предыдущий снимок ещё пишется);
    // update — шаг на кадр; is_iso_capture_busy — пока идёт загрузка/рендер (мир на паузе).
    bool start_iso_capture();
    void update_iso_capture();
    void cancel_iso_capture();
    bool is_iso_capture_busy() const { return m_iso_capture_ptr && m_iso_capture_ptr->is_busy(); }
    void render_iso_capture_overlay();

    void request_exit();
    void set_gameplay_input_active(bool active);
    void handle_gameplay_input();
    // simulation_active=false — полная пауза (меню паузы). player_actions=false — мир живёт,
    // но игрок стоит без управления (открыт инвентарь/контейнер/чат): физика, мобы, время идут.
    void update_gameplay(float delta_time, bool simulation_active, bool player_actions = true);
    void render_gameplay(bool show_gameplay_hud);
    // --- Чат (клавиша T или /) -----------------------------------------------------
    // Открытый чат забирает весь игровой ввод; мир при этом стоит на паузе, как у инвентаря.
    void open_chat(bool with_slash = false);
    void close_chat();
    bool is_chat_open() const { return m_chat_open; }
    void on_text_char(unsigned int codepoint);   // вызывается из GLFW char-callback
    void update_chat(float delta_time);          // таймеры строк + автоповтор Backspace
    void submit_chat_line();
    void chat_history_prev();
    void chat_history_next();
    void chat_autocomplete();                    // Tab
    void push_chat_message(const std::string& text, bool is_error = false);

    // --- API для Command_System --------------------------------------------------
    static constexpr int k_biome_search_radius = 3000;
    glm::vec3 get_player_position() const { return m_player_ptr->get_position(); }
    void teleport_player(const glm::vec3& pos);
    // Ищет ближайший биом по имени (частичное совпадение, без учёта регистра, '_' = пробел).
    bool locate_biome(const std::string& name, glm::vec3& out_pos, std::string& out_biome_name);
    // Время суток в тиках Minecraft (0..23999): 0 рассвет, 6000 полдень, 12000 закат, 18000 полночь.
    int get_day_time_ticks() const;
    void set_day_time_ticks(int ticks);
    void add_day_time_ticks(int ticks);
    int kill_all_mobs();
    void kill_player();
    void goto_spawn() { teleport_player(m_spawn_position); }
    void toggle_fly() { m_player_ptr->toggle_flying(); }
    void set_game_mode(Game_Mode mode);
    // Возвращает, сколько предметов реально влезло в инвентарь (только Survival).
    int give_item(Block_Types type, int count);
    bool set_block_at(int x, int y, int z, Block_Types type);
    int summon_pigs(int count);
    int summon_cows(int count);
    // Команда /breed: сразу «влюбляет» всех взрослых животных вида (pig|cow|horse) — удобно проверять
    // размножение без кормёжки. Возвращает, сколько животных затронуто.
    int breed_all(const std::string& species);
    int summon_horses(int count);
    bool is_riding_horse() const { return m_ridden_horse != nullptr; }
    int clear_dropped_items();
    void run_command(const std::string& line_without_slash);

    const std::string& get_generation_folder() const { return m_generation_folder; }
    void toggle_generation_folder();
    Game_Mode get_world_game_mode() const { return m_game_mode; }
};

#endif //OPTICRAFT_GAME_H
