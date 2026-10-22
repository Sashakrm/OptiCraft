//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_CONFIG_H
#define OPTICRAFT_CONFIG_H

#include <cstdint>

namespace Config {
    // Мир
    constexpr int chunk_size = 32;
    // Высота ОДНОГО чанка-куба (не всего мира!). Внутреннее хранение блоков, layer culling
    // и Y-секции меша (см. ниже) работают в пределах именно этой высоты и не знают о том,
    // что чанки теперь стекуются по Y — это сделано намеренно, чтобы не трогать код чанка.
    constexpr int chunk_height = 32;
    // Фактический вертикальный диапазон мира. Верхняя граница включительна.
    constexpr int world_min_y = -256;
    constexpr int world_max_y = 1024;
    constexpr int world_height = world_max_y - world_min_y + 1;
    // Диапазон chunk-Y, покрывающий весь мир. Верхний чанк частичный: его локальный y=0
    // соответствует мировому Y=1024, остальные 31 слоя находятся уже за пределами мира.
    constexpr int world_min_chunk_y = world_min_y / chunk_height;
    constexpr int world_max_chunk_y = world_max_y / chunk_height;
    constexpr int world_height_chunks = world_max_chunk_y - world_min_chunk_y + 1;
    // Сколько вертикальных чанков держим загруженными вокруг игрока. Сам мир при этом
    // остаётся полностью процедурным в диапазоне -256..1024 и подгружается потоково.
    // Загружаем только ближайшие этажи. Дальние вертикальные чанки почти никогда
    // не видны и создавали огромный стартовый burst генерации и мешей.
    constexpr int vertical_load_radius_chunks = 8;
    // Секции по Y внутри чанка (как legacy Section) — используются для покомпонентного
    // frustum-culling меша и для layer-culling при постройке меша (см. Chunk::build_mesh).
    // Должно ровно делить chunk_height.
    constexpr int chunk_section_height = 8;
    static_assert(chunk_height % chunk_section_height == 0,
                  "chunk_height must be evenly divisible by chunk_section_height");
    constexpr int chunk_section_count = chunk_height / chunk_section_height;
    constexpr int chunk_load_radius = 5;
    // Допустимые значения дальности прорисовки (в чанках), которые перебирает
    // кнопка в меню настроек. chunk_load_radius — значение по умолчанию.
    constexpr int render_distance_min = 2;
    constexpr int render_distance_max = 48;
    // Дальность прорисовки травы и цветов, в чанках. Флора — самая мелкая и самая
    // многочисленная геометрия, вдалеке она вырождается в пиксельный шум, поэтому
    // её радиус задаётся отдельно от общей дальности. -1 = без ограничения.
    constexpr int flora_render_distance_chunks = 4;
    constexpr int flora_render_distance_max = 12;

    // Максимальный уровень света от блока (Sun Stone и т.п.), 0 = нет света. Свет затухает
    // на 1 за каждый блок пути через прозрачные соседние клетки (см. Chunk_Manager::propagate_light).
    // 14 подобрано под явное требование "радиус около 14 блоков".
    constexpr int max_light_level = 14;
    constexpr int decoration_overhang = 8;
    constexpr uint16_t seed =1327;

    // Жидкости: длительность одного тика симуляции (как 1/20 с в Minecraft) и потолок числа
    // обработанных клеток за кадр — чтобы затопление большой пещеры растянулось на несколько
    // кадров, а не подвесило игру.
    constexpr float fluid_tick_seconds = 0.05f;
    constexpr int fluid_max_updates_per_frame = 1500;

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
    constexpr float player_sneak_edge_drop = 0.6f;         // Shift: не шагать туда, где под ногами пусто глубже этого
    constexpr float player_fly_speed = 198.0f;
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

    // --- Выпавшие предметы ---------------------------------------------------------------
    // Через сколько секунд неподобранный предмет исчезает (2 минуты).
    constexpr float dropped_item_lifetime = 120.0f;
    // Сразу после выпадения предмет нельзя подобрать (как в Minecraft, 10 тиков).
    constexpr float dropped_item_pickup_delay = 0.5f;
    // Сторона хитбокса-куба предмета, блоков.
    constexpr float dropped_item_size = 0.25f;
    // Радиус подбора по горизонтали от центра игрока, блоков.
    constexpr float dropped_item_pickup_radius = 1.1f;
    // Скорость вращения на земле, рад/с (~ 1 оборот за 4 с).
    constexpr float dropped_item_spin_speed = 1.6f;
    // Предметы одного типа ближе этого расстояния сливаются в один стак.
    constexpr float dropped_item_merge_radius = 0.7f;
    // Жёсткий потолок на число предметов в мире: при превышении удаляются самые старые.
    constexpr int dropped_item_max_count = 300;

    // --- Земледелие и животноводство ----------------------------------------------------
    // Время роста одного ростка выбирается случайно при посадке (секунды).
    constexpr float crop_grow_seconds_min = 180.0f;
    constexpr float crop_grow_seconds_max = 360.0f;
    // Грядка влажная, если вода не дальше этого числа блоков; на влажной всё растёт быстрее.
    constexpr int farmland_water_radius = 4;
    constexpr float crop_wet_growth_multiplier = 2.0f;
    // Шанс (на 1000 колонок) и размер «дикой» грядки с морковью при генерации мира.
    constexpr int wild_carrot_patch_per_mille_plains = 1;
    constexpr int wild_carrot_patch_per_mille_forest = 1;
    // Размножение: детёныш растёт 15 минут, после рождения родители отдыхают 5 минут.
    constexpr float animal_baby_grow_seconds = 15.0f * 60.0f;
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

    // Камера
    constexpr float camera_fov = 70.0f;
    constexpr float camera_near = 0.1f;
    constexpr float camera_far = 1000.0f;
    constexpr float mouse_sensitivity = 0.35f;

    // Рендер
    constexpr float clear_color_r = 0.45f;
    constexpr float clear_color_g = 0.68f;
    constexpr float clear_color_b = 1.0f;
    constexpr bool vsync_enabled = false;

    // Окно
    constexpr int default_window_width = 1280;
    constexpr int default_window_height = 720;
    constexpr const char* default_window_title = "OptiCraft";

    // --- Изометрический снимок мира (F7) ------------------------------------------------
    // Снимается квадрат (2R+1) x (2R+1) чанков вокруг игрока: R = 22 -> 45 x 45 чанков.
    constexpr int iso_capture_radius_chunks = 14;
    // Сколько чанковых слоёв по Y грузить НИЖЕ самой низкой точки поверхности вокруг колонки
    // (толщина «среза» мира у границы кадра). Больше слоёв = толще срез, но больше памяти.
    constexpr int iso_capture_extra_depth_layers = 1;
    // Запас над самой высокой точкой колонки (блоки): деревья и прочие структуры.
    constexpr int iso_capture_top_margin_blocks = 32;
    // Угол камеры над горизонтом: atan(1/sqrt(2)) = 35.264 градуса — настоящая изометрия.
    constexpr float iso_capture_pitch_degrees = 35.264f;
    // Откуда смотрит камера: 45 градусов вокруг оси Y (с юго-востока на северо-запад).
    constexpr float iso_capture_yaw_degrees = 45.0f;
    // Наибольшая сторона итоговой картинки в пикселях. Картинка больше FBO склеивается из тайлов.
    constexpr int iso_capture_max_image_size = 8192;
    // Сторона одного тайла (FBO). Урезается до лимита видеокарты.
    constexpr int iso_capture_tile_size = 4096;

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

    // --- Вода: сопротивление, плавучесть, течение, воздух (игрок и мобы) -----------------
    // Вода — жидкость из Fluid_Simulator (state == Liquid). Все мобы и игрок сэмплируют её
    // через Liquid_Physics; разница только в флаге can_swim (см. Hostile_Stats/Passive_Stats).
    // Горизонтальная скорость внутри жидкости умножается на это значение (0.45 — ходьба по воде
    // примерно вдвое медленнее, как в Minecraft).
    constexpr float water_horizontal_drag = 0.45f;
    // Игрок и мобы, умеющие плавать: удерживаемый прыжок (или подъём) поднимает со скоростью
    // не выше этой (блоков/с); игрок может активно выплыть и выпрыгнуть из воды.
    constexpr float water_swim_up_speed = 8.0f;
    // Предельная скорость погружения тонущего/неплавающего тела.
    constexpr float water_sink_speed = -2.0f;
    // Скорость течения при полном перепаде уровней между соседними клетками жидкости.
    constexpr float water_flow_speed = 1.4f;
    // Воздух: сколько секунд можно держать голову под водой; над водой восстанавливается быстро.
    constexpr float breath_max_seconds = 15.0f;
    constexpr float breath_refill_per_second = 10.0f;
    // Когда воздух кончился, раз в интервал отнимается урон утопления.
    constexpr float drown_damage_interval = 1.0f;
    constexpr float drown_damage = 2.0f;

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

#endif //OPTICRAFT_CONFIG_H
