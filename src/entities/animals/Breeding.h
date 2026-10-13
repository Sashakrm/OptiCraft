//
// Breeding_State — общее для свиней, коров и лошадей состояние размножения:
//   - детёныш растёт Config::animal_baby_grow_seconds (15 минут), пока растёт — уменьшен в размере
//     и не размножается (лошадь-жеребёнка нельзя оседлать);
//   - после кормёжки взрослое животное «влюблено» Config::animal_love_seconds: если рядом
//     другое влюблённое того же вида, они идут друг к другу и рождают детёныша;
//   - после рождения у обоих родителей перезарядка Config::animal_breed_cooldown_seconds (5 минут).
// Саму логику поиска пары и рождения ведёт Game (шаблон update_breeding), здесь только таймеры.
//
#ifndef OPTICRAFT_BREEDING_H
#define OPTICRAFT_BREEDING_H

#include <algorithm>
#include "utils/Config.h"

struct Breeding_State {
    float grow_left = 0.0f;      // > 0 — детёныш; секунд до взросления
    float love_left = 0.0f;      // > 0 — готов к размножению (накормлен)
    float cooldown_left = 0.0f;  // > 0 — недавно рожал, снова кормить нельзя

    void update(float dt) {
        grow_left = std::max(0.0f, grow_left - dt);
        love_left = std::max(0.0f, love_left - dt);
        cooldown_left = std::max(0.0f, cooldown_left - dt);
    }
    bool is_baby() const { return grow_left > 0.0f; }
    bool in_love() const { return love_left > 0.0f && !is_baby(); }
    // Можно ли сейчас накормить, чтобы запустить размножение.
    bool can_be_fed() const { return !is_baby() && cooldown_left <= 0.0f && love_left <= 0.0f; }
    void make_baby() { grow_left = Config::animal_baby_grow_seconds; }
    void start_love() { love_left = Config::animal_love_seconds; }
    void finish_breeding() { love_left = 0.0f; cooldown_left = Config::animal_breed_cooldown_seconds; }
    // 0.5 у новорождённого -> 1.0 у взрослого (плавно за 15 минут).
    float size_factor() const {
        if (grow_left <= 0.0f) return 1.0f;
        return 0.5f + 0.5f * (1.0f - grow_left / Config::animal_baby_grow_seconds);
    }
};

#endif //OPTICRAFT_BREEDING_H
