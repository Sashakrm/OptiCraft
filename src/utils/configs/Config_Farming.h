#pragma once


namespace Config {
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
}
