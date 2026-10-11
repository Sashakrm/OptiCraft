//
// Horse_AI — конечный автомат поведения лошади (свободная жизнь + приручение).
// Портировано из гайда по ИИ коня (см. чат), переведено на delta_time, как и Pig_AI.
//
// Это ЧИСТО поведенческий слой: потребности (голод/энергия/страх) и выбор состояния.
// Перемещение, физику, езду и дрифт делает src/entities/Horse.h, который держит Horse_AI + Mob.
//
#ifndef OPTICRAFT_HORSE_AI_H
#define OPTICRAFT_HORSE_AI_H

#include <algorithm>
#include <glm/glm.hpp>
#include <random>
#include <string>

enum class Horse_State {
    Idle,     // стоит
    Wander,   // просто ходит
    Graze,    // щиплет траву
    Happy,    // радуется
    Scared,   // напуган (игрок резко подошёл)
    Sleep,    // спит
    Taming,   // на ней сидит игрок, но она не приручена — брыкается
    Ridden,   // на ней сидит игрок, приручена — слушается
    Flee      // убегает (паника)
};

class Horse_AI {
public:
    explicit Horse_AI(std::string name);

    // Свободная жизнь (нет всадника): потребности + экстренные переходы + выбор следующего занятия.
    void update_free(float delta_time);
    // Пока на лошади сидят: потребности не тикают (кроме затухания страха), состояние задаёт Horse.
    void update_rider(float delta_time);

    Horse_State get_state() const { return m_state; }
    float get_state_time() const { return m_state_timer; }
    std::string get_state_name() const;
    void enter_state(Horse_State state);

    // Страх растёт от угроз; fear_source — откуда бежать.
    void add_fear(float amount, const glm::vec3& fear_source);
    void set_fear(float value) { m_fear = std::clamp(value, 0.0f, 100.0f); }
    float get_fear() const { return m_fear; }
    const glm::vec3& get_fear_source() const { return m_fear_source; }

    float get_hunger() const { return m_hunger; }
    float get_energy() const { return m_energy; }
    void drain_energy(float amount) { m_energy = std::clamp(m_energy - amount, 0.0f, 100.0f); }
    const std::string& get_name() const { return m_name; }

    // Состояния, которые нельзя прервать рулеткой решений.
    static bool is_urgent(Horse_State state);

private:
    std::string m_name;
    Horse_State m_state = Horse_State::Idle;
    float m_state_timer = 0.0f;
    float m_next_decision = 2.0f;

    float m_hunger = 100.0f; // 0 = голодна
    float m_energy = 100.0f; // 0 = хочет спать
    float m_fear = 0.0f;     // 0..100
    glm::vec3 m_fear_source{0.0f};

    std::mt19937 m_rng;

    void choose_next_idle_state();
    float rand01();
    float rand_range(float lo, float hi);
};

#endif //OPTICRAFT_HORSE_AI_H
