//
// Pig_AI — простой конечный автомат поведения свиньи: голод/энергия/счастье
// двигают переходы между состояниями (Idle/Hungry/Eating/Sleepy/Sleeping/
// Walking/Happy). Портировано из наброска, который скинул пользователь
// (сгенерирован DeepSeek), под конвенции этого движка + переведено на
// delta_time вместо "один тик — один вызов": иначе на 60 FPS свинья
// голодала/уставала бы в 60 раз быстрее, чем в оригинальной консольной версии.
//
// Это ЧИСТО поведенческий слой — ничего не знает про GL/Mob/позицию в мире.
// Физическую сторону (перемещение, модель, анимация) собирает src/entities/Pig.h,
// который держит Pig_AI + Mob вместе.
//
#ifndef OPTICRAFT_PIG_AI_H
#define OPTICRAFT_PIG_AI_H

#include <string>
#include <random>

enum class Pig_State {
    Idle,     // Стоит без дела
    Hungry,   // Хочет есть
    Eating,   // Ест
    Sleepy,   // Хочет спать
    Sleeping, // Спит
    Walking,  // Гуляет
    Happy     // Радуется
};

class Pig_AI {
public:
    explicit Pig_AI(std::string name);

    // Копит delta_time и раз в TICK_INTERVAL_SECONDS секунд принимает одно
    // "решение" (эквивалент одного update() из оригинального наброска).
    void update(float delta_time);

    Pig_State get_state() const { return m_state; }
    std::string get_state_name() const;

    int get_hunger() const { return m_hunger; }
    int get_energy() const { return m_energy; }
    int get_happiness() const { return m_happiness; }
    const std::string& get_name() const { return m_name; }

private:
    // Одно "решение" ИИ в секунду — подобрано на глаз, чтобы поведение было
    // заметно глазом на 60 FPS, но не мигало между состояниями каждый кадр.
    static constexpr float TICK_INTERVAL_SECONDS = 1.0f;

    std::string m_name;
    Pig_State m_state = Pig_State::Idle;

    int m_hunger;
    int m_energy;
    int m_happiness;

    float m_tick_accumulator = 0.0f;
    // Свой генератор на свинью вместо std::rand()/std::srand(time()) из
    // оригинального наброска — тот пересевал ГЛОБАЛЬНЫЙ рандом при каждом
    // создании свиньи, что при спавне нескольких свиней подряд в одном тике
    // давало им одинаковые "случайные" решения.
    std::mt19937 m_rng;

    void set_state(Pig_State new_state);
    void tick(); // логика одного "решения" — как update() в оригинале
    int roll_percent(); // 0..99, замена std::rand() % 100
};

#endif //OPTICRAFT_PIG_AI_H
