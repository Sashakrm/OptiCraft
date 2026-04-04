//
// Created by noktemor on 28.03.2026.
//

#include "Chunk.h"
#include "World_Generator.h"
#include "Biome_Types.h"
#include <cmath>
#include <algorithm>

// === Конструктор ===
World_Generator::World_Generator(int seed)
    : m_seed(seed)
    , m_rng(static_cast<unsigned>(seed))
    , m_terrain_noise(seed, 6, 0.5f, 2.0f)        // 6 октав для детального рельефа
    , m_detail_noise(seed + 100, 4, 0.4f, 2.5f)    // Детали поверхности
    , m_cave_noise(seed + 200, 4, 0.6f, 3.0f)      // Пещеры
    , m_ore_noise(seed + 300, 3, 0.5f, 2.0f)       // Руды
    , m_biome_gen(seed)
    , m_terrain_scale(Config::terrain_base_scale)
    , m_terrain_amplitude(Config::terrain_height_multiplier)
{}

// === Конвертация координат ===
int World_Generator::world_to_local_y(int world_y) const {
    // Сдвигаем мир так, чтобы 0 = минимальный Y чанка
    return world_y - Config::world_min_y;
}

int World_Generator::local_to_world_y(int local_y, int chunk_base_y) const {
    return chunk_base_y * Config::chunk_height + local_y + Config::world_min_y;
}

// === Основная генерация чанка ===
void World_Generator::generate_chunk(Chunk* chunk) {
    int chunk_cx = chunk->get_chunk_x();
    int chunk_cz = chunk->get_chunk_z();

    // Мировые координаты начала чанка
    int chunk_world_x = chunk_cx * Config::chunk_size;
    int chunk_world_z = chunk_cz * Config::chunk_size;

    // 🔄 Для каждой колонки (X, Z) в чанке
    for (int local_z = 0; local_z < Config::chunk_size; ++local_z) {
        for (int local_x = 0; local_x < Config::chunk_size; ++local_x) {
            int world_x = chunk_world_x + local_x;
            int world_z = chunk_world_z + local_z;

            // 1️⃣ Определяем биом
            Biome_Type biome = m_biome_gen.get_biome(world_x, world_z);
            const auto& biome_props = get_biome_props(biome);

            // 2️⃣ Генерируем высоту поверхности
            float base_height = m_terrain_noise.get_height_noise(
                world_x * m_terrain_scale,
                world_z * m_terrain_scale
            );
            float detail = m_detail_noise.get_height_noise(
                world_x * Config::terrain_detail_scale,
                world_z * Config::terrain_detail_scale
            );

            // Итоговая высота: базовая + детали + амплитуда биома
            int surface_y = Config::world_sea_level +
                           static_cast<int>((base_height * 0.7f + detail * 0.3f) *
                                           biome_props.terrain_amplitude);

            // Ограничиваем в рамках мира
            surface_y = std::clamp(surface_y, Config::world_min_y, Config::world_max_y);

            // 3️⃣ Заполняем вертикальную колонку
            for (int local_y = 0; local_y < Config::chunk_height; ++local_y) {
                int world_y = local_y + Config::world_min_y;  // Конверсия в мировые координаты

                Block_Type block = determine_block(
                    world_x, world_y, world_z,
                    surface_y, biome, biome_props
                );

                chunk->set_block(local_x, local_y, local_z, block);
            }

            // 4️⃣ Генерация деревьев на поверхности (только если это суша)
            if (surface_y > Config::world_sea_level) {
                std::uniform_real_distribution<float> dist(0.0f, 1.0f);
                if (dist(m_rng) < biome_props.tree_density) {
                    int surface_local_y = world_to_local_y(surface_y);
                    if (surface_local_y >= 0 && surface_local_y < Config::chunk_height) {
                        generate_tree(chunk, local_x, local_z, surface_local_y, biome);
                    }
                }
            }
        }
    }
}

// === Определение типа блока для конкретной позиции ===
Block_Type World_Generator::determine_block(
    int wx, int wy, int wz,
    int surface_y, Biome_Type biome, const Biome_Properties& props
) {
    // 🌊 Океан: вода ниже sea_level, если поверхность тоже ниже
    if (wy < Config::world_sea_level && surface_y < Config::world_sea_level) {
        // Дно океана: песок на мелководье, камень глубже
        int depth = Config::world_sea_level - wy;
        if (depth <= 3) {
            return props.underwater_block;  // Обычно песок
        }
        // Пещеры под водой
        if (is_cave(wx, wy, wz)) {
            return Block_Type::Air;
        }
        return Block_Type::Stone;
    }

    // 🏖️ Пляж: переход между водой и сушей
    if (wy == Config::world_sea_level && surface_y <= Config::world_sea_level + 1) {
        return Block_Type::Sand;
    }

    // 🏔️ Поверхность суши
    if (wy == surface_y && surface_y > Config::world_sea_level) {
        // Снег выше снежной линии
        if (props.has_snow && wy >= props.snow_line) {
            return Block_Type::Snow;  // Нужно добавить в Block_Types
        }
        return props.surface_block;  // Трава/песок/мицелий и т.д.
    }

    // 🪨 Под поверхностью: несколько слоёв
    if (wy < surface_y && wy >= surface_y - 3) {
        return props.subsurface_block;  // Обычно Dirt
    }

    // 🕳️ Пещеры (3D шум)
    if (is_cave(wx, wy, wz)) {
        return Block_Type::Air;
    }

    // 🪨 Глубокий камень
    if (wy < Config::world_sea_level - 10) {
        return props.stone_block;  // Обычно Stone
    }

    // 🌊 Вода на уровне моря (если поверхность выше)
    if (wy <= Config::world_sea_level && surface_y > Config::world_sea_level) {
        return Block_Type::Water;
    }

    // По умолчанию — воздух
    return Block_Type::Air;
}

// === Проверка пещеры ===
bool World_Generator::is_cave(int world_x, int world_y, int world_z) const {
    // Получаем значение 3D шума
    float cave_value = m_cave_noise.get_cave_noise(
        world_x * 0.04f,  // Масштаб для крупных пещер
        world_y * 0.04f,
        world_z * 0.04f
    );

    // Порог: чем выше значение — тем больше шанс пещеры
    // Добавляем вариативность по высоте (больше пещер глубоко)
    float depth_factor = 1.0f - static_cast<float>(world_y - Config::world_min_y) /
                                    static_cast<float>(Config::world_vertical_range);

    float threshold = 0.55f + depth_factor * 0.1f;  // 0.55..0.65

    return cave_value > threshold;
}

// === Генерация дерева ===
void World_Generator::generate_tree(Chunk* chunk, int local_x, int local_z,
                                   int surface_y, Biome_Type biome) {
    const auto& props = get_biome_props(biome);

    // Параметры дерева в зависимости от биома
    int trunk_height = 4 + (m_rng() % 4);  // 4-7 блоков
    int canopy_radius = (biome == Biome_Type::Jungle) ? 3 : 2;

    // 🪵 Ствол
    for (int y = 0; y < trunk_height; ++y) {
        int tree_y = surface_y + y;
        if (tree_y >= Config::chunk_height) break;

        // Проверяем, что не заменяем воду/другие важные блоки
        if (chunk->get_block(local_x, tree_y, local_z) == Block_Type::Air) {
            chunk->set_block(local_x, tree_y, local_z, Block_Type::Wood);
        }
    }

    // 🍃 Листва (сфера/куб)
    int canopy_base = surface_y + trunk_height - 1;
    for (int dy = 0; dy <= canopy_radius + 1; ++dy) {
        int radius = (dy == 0) ? canopy_radius - 1 :
                     (dy == canopy_radius + 1) ? 1 : canopy_radius;

        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                // Пропускаем углы для более естественной формы
                if (std::abs(dx) == radius && std::abs(dz) == radius) continue;

                int lx = local_x + dx;
                int ly = canopy_base + dy;
                int lz = local_z + dz;

                // Проверка границ чанка
                if (lx < 0 || lx >= Config::chunk_size ||
                    ly < 0 || ly >= Config::chunk_height ||
                    lz < 0 || lz >= Config::chunk_size) {
                    continue;
                }

                // Ставим листву только в воздух (не заменяем ствол!)
                if (chunk->get_block(lx, ly, lz) == Block_Type::Air) {
                    chunk->set_block(lx, ly, lz, Block_Type::Leaves);
                }
            }
        }
    }
}

// === Генерация рудной жилы ===
void World_Generator::generate_ore_vein(Chunk* chunk, int local_x, int local_y, int local_z,
                                       Block_Type ore_type, int size, float chance) {
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    for (int i = 0; i < size; ++i) {
        if (dist(m_rng) > chance) continue;

        // Смещение от центра жилы
        int ox = local_x + (m_rng() % 5) - 2;
        int oy = local_y + (m_rng() % 3) - 1;
        int oz = local_z + (m_rng() % 5) - 2;

        // Границы
        if (ox < 0 || ox >= Config::chunk_size ||
            oy < 0 || oy >= Config::chunk_height ||
            oz < 0 || oz >= Config::chunk_size) {
            continue;
        }

        // Заменяем только камень
        if (chunk->get_block(ox, oy, oz) == Block_Type::Stone) {
            chunk->set_block(ox, oy, oz, ore_type);
        }
    }
}

// === Публичные утилиты ===
int World_Generator::get_surface_height(int world_x, int world_z) const {
    Biome_Type biome = m_biome_gen.get_biome(world_x, world_z);
    const auto& props = get_biome_props(biome);

    float base = m_terrain_noise.get_height_noise(
        world_x * m_terrain_scale, world_z * m_terrain_scale
    );
    float detail = m_detail_noise.get_height_noise(
        world_x * Config::terrain_detail_scale, world_z * Config::terrain_detail_scale
    );

    return Config::world_sea_level +
           static_cast<int>((base * 0.7f + detail * 0.3f) * props.terrain_amplitude);
}

void World_Generator::set_terrain_params(float scale, float amplitude) {
    m_terrain_scale = scale;
    m_terrain_amplitude = amplitude;
}

void World_Generator::set_cave_density(float density) {
    // density: 0.0 (нет пещер) - 1.0 (всё пещеры)
    // Преобразуем в порог для шума: больше density = меньше порог = больше пещер
    m_cave_noise.set_persistence(0.5f + density * 0.3f);
}