#pragma once


namespace Config {
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

}
