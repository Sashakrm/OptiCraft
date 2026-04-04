//
// Created by noktemor on 30.03.2026.
//

#ifndef OPTICRAFT_BIOME_TYPES_H
#define OPTICRAFT_BIOME_TYPES_H



#include <string>
#include <array>
#include <glm/glm.hpp>
#include "Block_Types.h"

// Поддержка до 64 биомов (можно расширить до 256)
constexpr size_t MAX_BIOMES = 64;

enum class Biome_Type : uint8_t {
    // 🌊 Водные
    Ocean = 0,
    Deep_Ocean,
    Beach,

    // 🌿 Равнины
    Plains,
    Sunflower_Plains,
    Savanna,

    // 🌲 Леса
    Forest,
    Flower_Forest,
    Birch_Forest,
    Dark_Forest,
    Jungle,

    // 🏔️ Горы
    Mountains,
    Gravelly_Mountains,
    Snowy_Mountains,
    Stone_Shore,

    // ❄️ Холодные
    Tundra,
    Ice_Spikes,
    Frozen_Ocean,

    // 🏜️ Сухие
    Desert,
    Badlands,

    // 🍄 Специальные
    Mushroom_Fields,
    Swamp,

    // Заглушка для будущих биомов
    Count = 32  // ✅预留 место для 30-50 биомов
};

struct Biome_Properties {
    std::string name;

    // 🎨 Визуал (для будущего шейдера)
    glm::vec3 fog_color;
    glm::vec3 water_color;
    glm::vec3 grass_color;
    glm::vec3 foliage_color;

    // 🌡️ Параметры для классификации
    float temperature;      // 0.0 (холодно) - 1.0 (жарко)
    float humidity;         // 0.0 (сухо) - 1.0 (влажно)
    float elevation;        // 0.0 (низко) - 1.0 (высоко)

    // 🏔️ Генерация рельефа
    float terrain_scale;
    float terrain_amplitude;
    float surface_roughness;

    // 🌳 Растительность
    float tree_density;
    float flower_density;
    float grass_density;

    // 🧱 Блоки
    Block_Type surface_block;
    Block_Type subsurface_block;
    Block_Type stone_block;
    Block_Type underwater_block;

    // 🕳️ Пещеры
    float cave_density;
    float cave_size;

    // ❄️ Снег/лёд
    float snow_line;        // Высота, выше которой снег
    bool has_snow;
    bool has_ice;

    // ✅ Конструктор для инициализации списком
    Biome_Properties() = default;

    Biome_Properties(
        std::string n, glm::vec3 fog, glm::vec3 water, glm::vec3 grass, glm::vec3 foliage,
        float temp, float hum, float elev, float t_scale, float t_amp, float rough,
        float tree_d, float flower_d, float grass_d,
        Block_Type surf, Block_Type sub, Block_Type stone, Block_Type underwater,
        float cave_d, float cave_s, float snow_l, bool has_s, bool has_i
    ) : name(std::move(n)), fog_color(fog), water_color(water), grass_color(grass), foliage_color(foliage),
        temperature(temp), humidity(hum), elevation(elev),
        terrain_scale(t_scale), terrain_amplitude(t_amp), surface_roughness(rough),
        tree_density(tree_d), flower_density(flower_d), grass_density(grass_d),
        surface_block(surf), subsurface_block(sub), stone_block(stone), underwater_block(underwater),
        cave_density(cave_d), cave_size(cave_s), snow_line(snow_l), has_snow(has_s), has_ice(has_i) {}
};

//  Реестр биомов — легко добавлять новые
class Biome_Registry {
private:
    std::array<Biome_Properties, MAX_BIOMES> m_properties;

    Biome_Registry();  // Singleton

public:
    static Biome_Registry& get_instance();

    const Biome_Properties& get(Biome_Type type) const;
    Biome_Type from_name(const std::string& name) const;

    // Для расширения: зарегистрировать новый биом
    void register_biome(Biome_Type type, const Biome_Properties& props);
};

// Удобная функция доступа
inline const Biome_Properties& get_biome_props(Biome_Type type) {
    return Biome_Registry::get_instance().get(type);
}



#endif //OPTICRAFT_BIOME_TYPES_H
