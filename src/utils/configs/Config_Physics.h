#pragma once


namespace Config {
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

}
