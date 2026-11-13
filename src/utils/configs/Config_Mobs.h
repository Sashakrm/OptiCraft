#pragma once


namespace Config {
    constexpr float animal_breed_cooldown_seconds = 5.0f * 60.0f;
    // Сколько секунд животное остаётся «влюблённым» после кормёжки (за это время должен быть накормлен и партнёр).
    constexpr float animal_love_seconds = 45.0f;
    // Дальше этого расстояния партнёры друг друга не ищут.
    constexpr float animal_breed_search_radius = 12.0f;
    // Коровы (как свиньи: держим минимум рядом с игроком).
    constexpr int min_cow_count = 6;
    constexpr float cow_spawn_radius_min = 6.0f;
    constexpr float cow_spawn_radius_max = 18.0f;
    constexpr float cow_walk_speed = 1.0f;
    constexpr float cow_width = 0.9f;
    constexpr float cow_height = 1.3f;
    constexpr float cow_max_health = 10.0f;
    constexpr float cow_panic_seconds = 3.0f;
    constexpr float cow_panic_speed = 3.0f;
    constexpr float cow_invulnerable_seconds = 0.5f;
    constexpr float cow_death_seconds = 0.8f;
    constexpr float cow_respawn_delay = 25.0f;
    constexpr int cow_drop_min = 1;
    constexpr int cow_drop_max = 3;
    constexpr int horse_drop_min = 1;
    constexpr int horse_drop_max = 3;
    // Кожа: падает вместе с мясом у коров и лошадей (Survival, при убийстве).
    constexpr int cow_leather_min = 1;
    constexpr int cow_leather_max = 2;
    constexpr int horse_leather_min = 1;
    constexpr int horse_leather_max = 2;

    // --- Враждебные мобы (Survival, по умолчанию появляются вокруг игрока) -----------------
    // Зомби и скелет: видят игрока в radius (вдвое меньше, если игрок крадётся), бегут к нему и
    // бьют в упор. Заметившие игрока тревожат других врагов рядом (alert_radius) — идут толпой.
    // После того как игрок скрылся, враг помнит его memory_seconds и идёт к последней точке.
    constexpr float hostile_memory_seconds = 8.0f;
    constexpr float hostile_alert_radius = 16.0f;
    // Если за hostile_stuck_check_seconds погоня не сдвинула врага, он на hostile_detour_seconds уходит боком.
    constexpr float hostile_stuck_check_seconds = 0.5f;
    constexpr float hostile_detour_seconds = 1.0f;
    constexpr float hostile_attack_height_tolerance = 2.0f; // по вертикали удар достаёт, если разница не больше
    // Зомби. Модель assets/mobs/zombie.glb: клипы Idle, Walk и (необязательно) Attack.
    constexpr int min_zombie_count = 4;
    constexpr float zombie_spawn_radius_min = 16.0f;
    constexpr float zombie_spawn_radius_max = 28.0f;
    constexpr float zombie_walk_speed = 1.0f;
    constexpr float zombie_run_speed = 2.8f;
    constexpr float zombie_detect_range = 16.0f;
    constexpr float zombie_width = 0.6f;
    constexpr float zombie_height = 1.9f;
    constexpr float zombie_max_health = 20.0f;
    constexpr float zombie_attack_damage = 3.0f;         // HP за удар
    constexpr float zombie_attack_reach = 1.6f;          // горизонтально, блоков
    constexpr float zombie_attack_cooldown = 1.0f;
    constexpr float zombie_invulnerable_seconds = 0.4f;
    constexpr float zombie_death_seconds = 0.8f;
    // Скелет. Модель assets/mobs/skeleton.glb: клипы Idle, Walk и (необязательно) Attack.
    constexpr int min_skeleton_count = 3;
    constexpr float skeleton_spawn_radius_min = 16.0f;
    constexpr float skeleton_spawn_radius_max = 28.0f;
    constexpr float skeleton_walk_speed = 1.0f;
    constexpr float skeleton_run_speed = 2.6f;
    constexpr float skeleton_detect_range = 16.0f;
    constexpr float skeleton_width = 0.6f;
    constexpr float skeleton_height = 1.9f;
    constexpr float skeleton_max_health = 16.0f;
    constexpr float skeleton_attack_damage = 2.0f;
    constexpr float skeleton_attack_reach = 1.6f;
    constexpr float skeleton_attack_cooldown = 1.2f;
    constexpr float skeleton_invulnerable_seconds = 0.4f;
    constexpr float skeleton_death_seconds = 0.8f;
    // После убийства враждебного моба новый появляется не раньше, чем через это время.
    constexpr float hostile_respawn_delay = 20.0f;
    // Паук. Модель assets/mobs/spider.glb: клипы Idle, Walk и (необязательно) Attack.
    // Днём и ночью дерётся с лошадьми; на игрока нападает только ночью (см. night_start_normalized).
    constexpr int min_spider_count = 2;
    constexpr float spider_spawn_radius_min = 16.0f;
    constexpr float spider_spawn_radius_max = 28.0f;
    constexpr float spider_walk_speed = 1.2f;
    constexpr float spider_run_speed = 3.0f;
    constexpr float spider_detect_range = 16.0f;
    constexpr float spider_width = 0.9f;
    constexpr float spider_height = 0.8f;
    constexpr float spider_max_health = 16.0f;
    constexpr float spider_attack_damage = 2.0f;         // HP за удар (по игроку и по лошади)
    constexpr float spider_attack_reach = 1.4f;
    constexpr float spider_attack_cooldown = 1.0f;
    constexpr float spider_invulnerable_seconds = 0.4f;
    constexpr float spider_death_seconds = 0.8f;
    // Ночь начинается, когда нормализованное время суток >= этого значения (0.5 — закат, 0 — рассвет).
    constexpr float night_start_normalized = 0.5f;
    // Лошадь замечает паука и бежит к нему на этом расстоянии (блоков).
    constexpr float horse_hunt_range = 12.0f;

    // Мобы
    // Игра поддерживает у игрока не меньше этого числа свиней одновременно —
    // если меньше, при каждом апдейте доспавнивается недостающее количество
    // рядом с игроком (см. Mobs_Update::spawn_missing_pigs).
    constexpr int min_pig_count = 8;
    // Радиус (в блоках, по XZ) вокруг игрока, в котором рождаются новые свиньи.
    constexpr float pig_spawn_radius_min = 5.0f;
    constexpr float pig_spawn_radius_max = 15.0f;
    constexpr float pig_walk_speed = 1.2f;
    constexpr float pig_width = 0.6f;
    constexpr float pig_height = 0.9f;
    constexpr float pig_max_health = 10.0f;
    // После удара свинья убегает от игрока pig_panic_seconds секунд со скоростью pig_panic_speed.
    constexpr float pig_panic_seconds = 3.0f;
    constexpr float pig_panic_speed = 3.0f;
    // Неуязвимость после попадания, секунды — защита от многократного урона за один клик.
    constexpr float pig_invulnerable_seconds = 0.5f;
    // Длительность анимации падения на бок после смерти, секунды.
    constexpr float pig_death_seconds = 0.8f;
    // После убийства свиньи новые не появляются столько секунд (потом досоздаются до min_pig_count).
    constexpr float pig_respawn_delay = 25.0f;
    // Сколько мяса выпадает из свиньи (включительно).
    constexpr int pig_drop_min = 1;
    constexpr int pig_drop_max = 3;

    // Овцы — пассивные, как коровы (блуждание, паника, кормление пшеницей), отдельные параметры.
    constexpr int min_sheep_count = 4;
    constexpr float sheep_spawn_radius_min = 6.0f;
    constexpr float sheep_spawn_radius_max = 18.0f;
    constexpr float sheep_walk_speed = 0.9f;
    constexpr float sheep_width = 0.8f;
    constexpr float sheep_height = 1.1f;
    constexpr float sheep_max_health = 8.0f;
    constexpr float sheep_panic_seconds = 3.0f;
    constexpr float sheep_panic_speed = 3.2f;
    constexpr float sheep_invulnerable_seconds = 0.5f;
    constexpr float sheep_death_seconds = 0.8f;
    constexpr float sheep_respawn_delay = 25.0f;
    // --- Лошади -----------------------------------------------------------------------
    // Логика появления как у свиней (см. Mobs_Update::spawn_missing_horses): держим минимум лошадей
    // рядом с игроком, но лошади редкие и рождаются дальше.
    constexpr int min_horse_count = 2;
    constexpr float horse_spawn_radius_min = 12.0f;
    constexpr float horse_spawn_radius_max = 30.0f;
    // Коллизия с миром (AABB по осям, без учёта поворота) — модель длиннее, чем шире.
    constexpr float horse_width = 1.0f;
    constexpr float horse_height = 1.7f;
    constexpr float horse_max_health = 20.0f;
    constexpr float horse_walk_speed = 1.6f;
    constexpr float horse_flee_speed = 7.0f;
    constexpr float horse_invulnerable_seconds = 0.5f;
    constexpr float horse_death_seconds = 1.0f;
    // Атака лошади (удар копытом/таран). Пока вызывается только вручную — см. Horse::attack.
    constexpr float horse_attack_damage = 3.0f;        // базовый урон (HP)
    constexpr float horse_attack_reach = 2.2f;         // до ближайшей точки хитбокса цели, блоков
    constexpr float horse_attack_cooldown = 1.2f;      // секунды между ударами
    constexpr float horse_attack_knockback = 6.0f;     // горизонтальный импульс цели
    constexpr float horse_attack_speed_bonus = 0.35f;  // +урон за каждый блок/сек скорости (таран)
    constexpr float horse_attack_max_damage = 10.0f;   // потолок урона
    constexpr float horse_respawn_delay = 60.0f;
    // Высота «седла» над ногами лошади, куда сажается игрок.
    constexpr float horse_seat_height = 1.15f;
    // Приручение: удержаться на неприручённой лошади (секунд) / шанс успеха от числа попыток.
    constexpr float horse_taming_seconds = 5.0f;
    // Езда и дрифт. Скорости — блоки/сек, повороты — рад/сек, grip — скорость гашения
    // бокового скольжения (чем больше, тем «цепче»; при ручнике grip падает почти до нуля).
    constexpr float horse_ride_jump_speed = 8.5f;    // начальная вертикальная скорость прыжка по пробелу (~1.8 блока)
    constexpr float horse_ride_max_speed = 13.0f;
    constexpr float horse_ride_reverse_speed = 3.0f;
    constexpr float horse_ride_accel = 12.0f;
    constexpr float horse_ride_brake = 18.0f;
    constexpr float horse_ride_turn_rate = 3.4f;
    constexpr float horse_ride_grip = 12.0f;
    constexpr float horse_drift_grip = 1.4f;
    constexpr float horse_drift_turn_multiplier = 2.0f;
    constexpr float horse_drift_min_speed = 4.0f;
    constexpr float horse_ride_energy_drain = 0.012f;
    // Общая гравитация для ВСЕХ мобов (не только свиней) — падение у всех устроено
    // одинаково, поэтому одна переменная на всех, а не копия на каждый вид моба.
    // Если конкретному мобу когда-нибудь понадобится другая скорость падения —
    // умножать эту базовую величину в его собственной логике, а не заводить рядом
    // ещё одну почти такую же константу.
    constexpr float mob_gravity = -20.0f;
    // Терминальная скорость падения — без неё моб, заспавненный высоко над
    // землёй, мог бы за один долгий кадр (лагающий момент, загрузка чанков)
    // провалиться сквозь несколько блоков подряд, не пересекая их в свипе.
    constexpr float mob_max_fall_speed = -40.0f;

}
