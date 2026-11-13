#pragma once

#include <algorithm>
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

    // Дальность по высоте: полуширина вертикального окна загрузки и прорисовки в чанках (слоёв
    // над и под игроком = значение; всего уровней = 2 * значение + 1). В настройках 0 = «Авто»:
    // значение считается от горизонтальной дальности (см. auto_vertical_render_distance).
    constexpr int vertical_render_min = 2;
    constexpr int vertical_render_max = 12;
    // Прежнее поведение окна по высоте (4 слоя вверх и вниз) сохраняется на малой и средней дальности.
    constexpr int vertical_render_auto_min = 4;
    // round(horizontal / 3), но не меньше vertical_render_auto_min: дальность 20 → по высоте 7.
    constexpr int auto_vertical_render_distance(int horizontal) {
        return std::clamp((2 * horizontal + 3) / 6, vertical_render_auto_min, vertical_render_max);
    }

    constexpr int max_light_level = 14;

    // Дальность прямой тени от непрозрачных блоков над клеткой (в блоках). Крыша ближе этого
    // расстояния закрывает небо (клетка темнеет), а дальше — не отбрасывает тени вообще: парящий
    // остров высоко над землёй не даёт тени на поверхность. Пещеры выше этого значения будут
    // освещены сверху, поэтому дальность не стоит делать слишком большой.
    constexpr int sky_shadow_range = 32;
    // Погода: сколько длится одно состояние (ясно / облачно / дождь), в секундах реального времени.
    constexpr float weather_min_duration = 180.0f;
    constexpr float weather_max_duration = 480.0f;

    // Враги в выживании появляются только там, где освещение ниже этого значения (0..15).
    constexpr int hostile_spawn_max_light = 3;   // спавн при свете < 4
    constexpr int decoration_overhang = 8;
    constexpr uint16_t seed =1327;

    // Жидкости: длительность одного тика симуляции (как 1/20 с в Minecraft) и потолок числа
    // обработанных клеток за кадр — чтобы затопление большой пещеры растянулось на несколько
    // кадров, а не подвесило игру.
    constexpr float fluid_tick_seconds = 0.05f;
    constexpr int fluid_max_updates_per_frame = 1500;

}
