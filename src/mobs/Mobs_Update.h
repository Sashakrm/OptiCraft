//
// Created by noktemor on 14.10.2026.
//
//
// Mobs_Update.h — все животные и враждебные мобы мира в одном модуле.
//
// Зачем он нужен. Раньше Game сам спавнил животных, вёл их размножение, считал бой, езду
// и отрисовку, и всё это было переплетено с вводом игрока, инвентарём и камерой. Из-за этого
// добавление нового моба задевало Game, а баги проявлялись в самых неожиданных местах.
// Теперь Game только передаёт мобам контекст (позиция игрока, ввод, инвентарь) и применяет
// результат (куда поставить игрока, сколько урона получить, что съесть, сесть или слезть).
// Логика каждого моба живёт в своём файле (entities/animals, entities/hostile), а здесь —
// только учёт, спавн, обнаружение игрока и связь с игроком. Новый моб добавляется сюда и в свой
// файл, без правок игрового цикла.
//
// Компромиссы. Каждый из них помечен в коде словом «КОМПРОМИСС».
//  * Лошади обновляются по кадрам (update_frame), а не в тиках. Камера берёт позицию седла в
//    этом же кадре, иначе при езде картинка отстаёт и дёргается. Цена: лошадь не
//    детерминирована по тикам, и её поведение зависит от частоты кадров.
//  * Игрок физически остаётся в Game. Мобы возвращают Mob_Result (куда поставить игрока, садиться
//     или слезать, сколько урона получить), а Game применяет его к Player и камере. Благодаря этому
//    физика игрока не знает о мобах.
//  * Ввод для езды (WASD, пробел, ctrl) собирает Game и передаёт сюда готовый Horse_Ride_Input.
//    Мобы не читают клавиатуру.
//  * Прицеливание и поиск цели идут по кадрам, потому что зависят от текущего луча взгляда.
//  * Обнаружение игрока врагами (дистанция и луч через блоки) считается здесь, а не в самих
//    врагах, потому что ему нужен доступ к миру и к другим врагам (тревога).
//
// Важно для GL. Все мобы держат GL-ресурсы. Их нужно уничтожить ДО glfwTerminate(), иначе
// деструктор упадёт на мёртвом контексте. Поэтому shutdown() здесь сначала очищает списки, а
// потом сбрасывает кэш моделей Mob. Game::shutdown вызывает его в нужном месте.
//
#ifndef OPTICRAFT_MOBS_UPDATE_H
#define OPTICRAFT_MOBS_UPDATE_H


#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "entities/animals/Pig.h"
#include "entities/animals/Passive_Animal.h"
#include "entities/animals/Horse.h"
#include "entities/hostile/Hostile_Mob.h"
#include "entities/items/Dropped_Item.h"
#include "entities/player/Inventory.h"
#include "world/blocks/Block_Types.h"

class Chunk_Manager;
class Shader;

// Что мобам нужно знать об игроке. Сам Player сюда не передаётся, чтобы мобы не зависели от него.
struct Mob_Player_View {
    glm::vec3 position{0.0f};   // ноги
    float eye_height = 1.62f;
    bool alive = true;
    bool sneaking = false;      // крадущегося игрока враги видят вдвое хуже
    float horizontal_speed = 0.0f; // блоков/с: от скорости зависит урон удара по мобу
};

// Проверка, свободно ли место для игрока (стоит ли там блок). Даётся Game'ом.
using Mob_Blocked_Fn = std::function<bool(const glm::vec3&)>;

// Результат, который Game должен применить к игроку и инвентарю после вызова мобов.
// Все поля по умолчанию «ничего не делать».
struct Mob_Result {
    bool set_player_position = false;  // переставить игрока в player_position (и камеру)
    glm::vec3 player_position{0.0f};
    bool set_riding = false;           // выставить флаг «сидит верхом» у Player
    bool riding = false;
    bool cancel_block_actions = false; // прервать добычу/установку блока (как при посадке и кормлении)
    bool consume_held_item = false;    // съесть один предмет из выбранного слота (только Survival)
    bool damage_held_tool = false;     // уменьшить прочность инструмента в руке на 1 (только Survival)
    float exhaustion = 0.0f;           // добавить истощение игроку (удар)
    int player_damage = 0;             // урон, который враги нанесли игроку в этом тике
};

// Вход для взаимодействия игрока с мобами (кормление, удар, посадка).
struct Mob_Interact_Input {
    float delta_time = 0.0f;
    glm::vec3 eye{0.0f};
    glm::vec3 look_dir{0.0f};
    bool block_targets_entity = false; // блок-взаимодействие сейчас нацелено на сущность
    bool right_click = false;
    bool left_click = false;
    bool survival = false;
    Block_Types held_block = Block_Types::Air;
    Item_Stack held_stack;
    Mob_Player_View player;
    std::function<void(const std::string&)> chat; // сообщения в чат (например, «Horse ate: …»)
};

class Mobs_Update {
public:
    // --- Жизненный цикл ---------------------------------------------------------------------
    void clear();        // смена мира или телепорт: убрать всех мобов и состояние езды
    void shutdown();     // выход из игры: clear() + сброс кэша моделей (порядок важен, см. выше)
    void reset_timers(); // новый мир: таймеры возрождения и атаки в ноль

    // Враги появляются только в Survival. Выключение убирает уже бегающих врагов.
    void set_hostiles_enabled(bool enabled);

    // --- Тики (мир и животные) --------------------------------------------------------------
    // Спавн и AI животных и врагов, размножение, уборка павших. Вызывается из Game::register_tick_systems.
    // Возвращает, сколько урона игрок получил от врагов в этом тике.
    Mob_Result update_tick(float dt, const Mob_Player_View& player, Chunk_Manager& chunks);

    // --- По кадрам (КОМПРОМИСС: лошади и езда) ----------------------------------------------
    // Лошади, посадка/высадка и синхронизация позиции всадника. Вызывается из Game::update_gameplay
    // до камеры, чтобы картинка не отставала от седла.
    Mob_Result update_frame(float dt, const Mob_Player_View& player, const Mob_Blocked_Fn& blocked,
                            Chunk_Manager& chunks);
    void set_ride_input(const Horse_Ride_Input& input) { m_ride_input = input; }
    void request_dismount() { m_dismount_requested = true; }
    bool is_riding() const { return m_ridden_horse != nullptr; }
    // place_beside=true — поставить игрока рядом с лошадью (обычная высадка); false — оставить
    // на месте (игрока уже переместили, например, телепорт или респавн).
    Mob_Result dismount(bool place_beside, const Mob_Player_View& player, const Mob_Blocked_Fn& blocked);

    // --- Взаимодействие игрока (по кадрам) ---------------------------------------------------
    // Ищет ближайшую цель на луче взгляда. Возвращает расстояние до неё или -1. Цель запоминается
    // для interact().
    float aim(const glm::vec3& eye, const glm::vec3& look_dir);
    Mob_Result interact(const Mob_Interact_Input& input, Dropped_Item_Manager& drops);

    // --- Команды ----------------------------------------------------------------------------
    int kill_all();                                              // /killall
    int summon_pigs(int count, const glm::vec3& around);         // /summon pig
    int summon_cows(int count, const glm::vec3& around);         // /summon cow
    int summon_sheep(int count, const glm::vec3& around);        // /summon sheep
    int summon_horses(int count, const glm::vec3& around);       // /summon horse
    int summon_zombies(int count, const glm::vec3& around);      // /summon zombie
    int summon_skeletons(int count, const glm::vec3& around);    // /summon skeleton
    // /breed: сразу «влюбляет» взрослых животных вида (pig|cow|sheep|horse). Возвращает число затронутых.
    int breed_all(const std::string& species);

    // --- Отрисовка --------------------------------------------------------------------------
    void render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient) const;

private:
    enum class Target_Kind { None, Pig, Passive, Horse, Hostile };

    void spawn_missing_pigs(const glm::vec3& player_position);
    void spawn_missing_passive(Passive_Kind kind, const glm::vec3& player_position);
    int summon_passive(Passive_Kind kind, int count, const glm::vec3& around);
    int summon_hostile(Hostile_Kind kind, int count, const glm::vec3& around);
    void spawn_missing_horses(const glm::vec3& player_position, Chunk_Manager& chunks);
    void spawn_missing_hostiles(Hostile_Kind kind, const glm::vec3& player_position, Chunk_Manager& chunks);
    void update_hostiles(float dt, const Mob_Player_View& player, Chunk_Manager& chunks, Mob_Result& result);
    void update_breeding();

    Mob_Result mount(Horse& horse, const Mob_Player_View& player);

    std::vector<std::unique_ptr<Pig>> m_pigs;
    std::vector<std::unique_ptr<Passive_Animal>> m_cows;
    std::vector<std::unique_ptr<Passive_Animal>> m_sheep;
    std::vector<std::unique_ptr<Horse>> m_horses;
    std::vector<std::unique_ptr<Hostile_Mob>> m_hostiles;
    bool m_hostiles_enabled = false;

    std::vector<std::unique_ptr<Passive_Animal>>& passive_list(Passive_Kind kind);

    // Таймеры возрождения: после убийства пауза, иначе животное появилось бы в тот же кадр
    // и мясо можно было бы фармить бесконечно. Индексы — static_cast<int>(Passive_Kind/Hostile_Kind).
    float m_pig_respawn_cooldown = 0.0f;
    std::array<float, 2> m_passive_respawn_cooldown{0.0f, 0.0f};
    std::array<float, 2> m_hostile_respawn_cooldown{0.0f, 0.0f};
    float m_horse_respawn_cooldown = 0.0f;
    float m_attack_cooldown = 0.0f;

    // Езда. Указатель на лошадь стабилен, потому что лошади лежат в unique_ptr.
    Horse* m_ridden_horse = nullptr;
    glm::vec3 m_last_seat_position{0.0f}; // где мы поставили игрока в прошлом кадре (чтобы заметить телепорт)
    Horse_Ride_Input m_ride_input;
    bool m_dismount_requested = false;

    // Текущая цель прицела (см. aim()).
    Target_Kind m_target_kind = Target_Kind::None;
    Pig* m_target_pig = nullptr;
    Passive_Animal* m_target_passive = nullptr;
    Horse* m_target_horse = nullptr;
    Hostile_Mob* m_target_hostile = nullptr;
};


#endif //OPTICRAFT_MOBS_UPDATE_H
