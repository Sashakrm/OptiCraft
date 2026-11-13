#pragma once


namespace Config {
    // Игрок
    constexpr float player_width = 0.6f;
    constexpr float player_height = 1.8f;
    constexpr float player_eye_height = 1.62f;
    constexpr float player_move_speed = 5.0f;
    constexpr float player_jump_force = 7.5f;
    constexpr float player_gravity = -20.0f;
    constexpr int player_max_health = 20;
    constexpr float player_crouch_height = 1.5f;
    constexpr float player_sneak_speed_multiplier = 0.3f;  // Shift: скорость ходьбы × это значение
    constexpr float player_step_height = 0.5f;             // подъём без прыжка: полублок (ступеньки, плиты)
    constexpr float player_sneak_edge_drop = 0.6f;         // Shift: не шагать туда, где под ногами пусто глубже этого
    constexpr float player_fly_speed = 198.0f;
    constexpr float double_jump_window_seconds = 0.35f;
    // Бег: удержание Ctrl на земле при движении вперёд. Не работает при приседе и в полёте.
    constexpr float player_sprint_speed_multiplier = 1.3f;
    constexpr float exhaustion_sprint_per_meter = 0.1f;   // бег тратит сытость в 10 раз быстрее ходьбы
    // Падение (Survival): первые fall_damage_safe_height блоков падения — без урона (как в
    // Minecraft), дальше — fall_damage_per_block HP за каждый блок сверху.
    constexpr float fall_damage_safe_height = 3.0f;
    constexpr float fall_damage_per_block = 2.0f;

    // --- Голод (Survival) --------------------------------------------------------------
    // Модель как в Minecraft 1.8: еда 0..20 + насыщение (запас, который тратится первым) +
    // истощение (копится от действий; каждые 4.0 списывает 1 насыщения, а если его нет — 1 еды).
    constexpr int player_max_food = 20;
    constexpr float player_initial_saturation = 5.0f;
    constexpr float hunger_exhaustion_threshold = 4.0f;
    constexpr float exhaustion_walk_per_meter = 0.01f;
    constexpr float exhaustion_jump = 0.05f;
    constexpr float exhaustion_break_block = 0.005f;
    constexpr float exhaustion_attack = 0.1f;
    constexpr float exhaustion_damage = 0.3f;       // получение урона тоже утомляет
    constexpr float exhaustion_regen = 3.0f;        // цена излечения 1 HP от сытости
    // Естественное лечение зависит от сытости (как в Minecraft 1.8):
    //  - еда == максимум и есть насыщение: быстрое лечение, раз в hunger_fast_regen_interval с
    //    восстанавливается min(насыщение, hunger_fast_regen_cap) / hunger_fast_regen_cap HP
    //    и столько же истощения тратится — чем больше насыщения, тем быстрее и дешевле по еде;
    //  - еда >= hunger_regen_min_food: обычное лечение, +1 HP раз в hunger_regen_interval с;
    //  - еда ниже — здоровье не восстанавливается вовсе.
    constexpr int hunger_regen_min_food = 18;
    constexpr float hunger_regen_interval = 4.0f;
    constexpr float hunger_fast_regen_interval = 0.5f;
    constexpr float hunger_fast_regen_cap = 6.0f;
    // Урон от голода при еде == 0: 1 HP раз в starvation_interval секунд. Здоровье, ниже
    // которого голод не добивает: 0 — голод убивает (как на Hard), 1 — как на Normal.
    constexpr float starvation_interval = 4.0f;
    constexpr int starvation_health_floor = 0;
    // Сколько секунд удерживать ПКМ, чтобы съесть один предмет (32 тика = 1.6 с в Minecraft).
    constexpr float eat_duration = 1.6f;

    // --- Добыча блоков (Survival) --------------------------------------------------------
    // Время добычи голыми руками = hardness * это значение (секунды). Блоки с hardness 0
    // (трава, цветы) ломаются мгновенно; отдельному блоку время можно задать полем
    // "break_time" в blocks.json. Инструментов пока нет (roadmap 1.3), поэтому все руками.
    constexpr float survival_break_seconds_per_hardness = 1.5f;

    // --- Бой -----------------------------------------------------------------------------
    constexpr float player_attack_reach = 3.5f;
    constexpr float player_attack_cooldown = 0.3f;
    constexpr float player_fist_damage = 1.0f;
    // Ctrl: короткий клик (меньше horse_sprint_tap_max_seconds) включает/выключает бег, удержание
    // дольше horse_drift_hold_seconds — ручник (дрифт). Бег держится, пока зажат W.
    constexpr float horse_sprint_tap_max_seconds = 0.2f;
    constexpr float horse_drift_hold_seconds = 0.2f;
    constexpr float horse_sprint_speed_multiplier = 1.5f;
    constexpr float horse_sprint_energy_multiplier = 1.5f; // бег быстрее утомляет

    // --- Импульс и скорость (игрок) --------------------------------------------------------
    // Горизонтальный импульс от инструментов затухает экспоненциально с этим коэффициентом.
    constexpr float impulse_drag = 2.2f;
    // Инструмент с полем "impulse_speed" в blocks.json (ПКМ): не раньше, чем через это время.
    constexpr float tool_impulse_cooldown = 0.6f;
    // Урон удара растёт от скорости игрока: + melee_speed_bonus_per_speed за каждый блок/с
    // сверх ходьбы (множитель урона; у каждого инструмента свой "speed_damage_scale" в blocks.json).
    // Верхнего предела нет.
    constexpr float melee_speed_bonus_per_speed = 0.12f;
    // Урон мобов по игроку растёт так же, но от скорости игрока сверх ходьбы (см. Hostile_Mob).
    constexpr float mob_speed_bonus_per_speed = 0.1f;
    // Абсолютный потолок горизонтальной скорости игрока (блоков/с), включая импульсы инструментов.
    constexpr float player_max_speed = 50.0f;

    // --- Броня (Survival) -------------------------------------------------------------------
    // Каждое очко защиты снижает урон от мобов на (1/25), но не больше чем на 20 очков (80%).
    constexpr int armor_points_cap = 20;
    constexpr float armor_points_divisor = 25.0f;

}
