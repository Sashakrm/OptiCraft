#pragma once


namespace Config {
    // --- Зелья-шарики и эффекты (см. entities/effects/Status_Effects.h) ----------------------
    // Регенерация: 1 HP за тик; тик каждые potion_regen_interval секунд (делится на уровень).
    constexpr float potion_regen_duration = 12.0f;
    constexpr float potion_regen_interval = 2.5f;
    // Скорость: +potion_speed_bonus за каждый уровень.
    constexpr float potion_speed_duration = 30.0f;
    constexpr float potion_speed_bonus = 0.25f;
    // Сила прыжка: +potion_jump_bonus за каждый уровень.
    constexpr float potion_jump_duration = 30.0f;
    constexpr float potion_jump_bonus = 0.35f;
    // Шарик летит с этой скоростью (блоков/с) и разбивается о блок, моба или через potion_thrown_lifetime.
    constexpr float potion_throw_speed = 14.0f;
    constexpr float potion_thrown_lifetime = 6.0f;
    // Радиус разлёта: все мобы и игрок внутри получают эффект.
    constexpr float potion_splash_radius = 2.5f;
    // Шарик попадает в моба, если его центр ближе этого расстояния к телу моба.
    constexpr float potion_mob_hit_radius = 0.8f;
}
