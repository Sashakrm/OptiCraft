#include "world/dimensions/Dimension.h"
#include "utils/Config.h"
#include <array>

int Dimension_Info::flat_top_y() const {
    int y = flat_base_y - 1;
    for (const Flat_Layer& layer : flat_layers) y += layer.thickness;
    return y;
}

Block_Types Dimension_Info::flat_block_at(int y) const {
    int bottom = flat_base_y;
    for (const Flat_Layer& layer : flat_layers) {
        if (y >= bottom && y < bottom + layer.thickness) return layer.block;
        bottom += layer.thickness;
    }
    return Block_Types::Air;
}

std::string Dimension_Info::storage_key(const std::string& generation_folder) const {
    if (id == Dimension_Id::Overworld) return generation_folder;
    return std::string(name) + "_" + generation_folder;
}

namespace {
    std::array<Dimension_Info, static_cast<size_t>(Dimension_Id::Count)> register_dimensions() {
        std::array<Dimension_Info, static_cast<size_t>(Dimension_Id::Count)> table;

        auto& overworld = table[static_cast<size_t>(Dimension_Id::Overworld)];
        overworld.id = Dimension_Id::Overworld;
        overworld.name = "overworld";
        overworld.display_name = "Overworld";
        overworld.terrain = Terrain_Mode::Noise;

        auto& flat = table[static_cast<size_t>(Dimension_Id::Flat_Test)];
        flat.id = Dimension_Id::Flat_Test;
        flat.name = "flat_test";
        flat.display_name = "Flat test";
        flat.terrain = Terrain_Mode::Flat;
        flat.decorations = false;
        flat.sky_islands = false;
        flat.flat_base_y = Config::world_min_y;
        flat.flat_layers = {
            {Block_Types::Deep_Stone, 1 - Config::world_min_y - 8},   // до Y = -8
            {Block_Types::Stone, 5},
            {Block_Types::Dirt, 2},
            {Block_Types::Grass, 1},
        };
        flat.spawn_height = flat.flat_top_y() + 1;

        auto& empty = table[static_cast<size_t>(Dimension_Id::Void_Test)];
        empty.id = Dimension_Id::Void_Test;
        empty.name = "void_test";
        empty.display_name = "Void test";
        empty.terrain = Terrain_Mode::Void;
        empty.time_mode = Time_Mode::Fixed;   // в пустоте времени суток нет: вечный полдень
        empty.fixed_time = 0.25f;
        empty.decorations = false;
        empty.sky_islands = false;
        empty.spawn_height = 64;

        // Супер-плоский: как в Minecraft — низ уходит под мир, сверху: камень, земля, трава.
        auto& superflat = table[static_cast<size_t>(Dimension_Id::Superflat)];
        superflat.id = Dimension_Id::Superflat;
        superflat.name = "superflat";
        superflat.display_name = "Superflat";
        superflat.terrain = Terrain_Mode::Flat;
        superflat.decorations = false;
        superflat.sky_islands = false;
        superflat.flat_base_y = Config::world_min_y;
        superflat.flat_layers = {
            {Block_Types::Deep_Stone, 0 - Config::world_min_y},       // до Y = -1
            {Block_Types::Stone, 3},                                  // Y 0..2
            {Block_Types::Dirt, 2},                                   // Y 3..4
            {Block_Types::Grass, 1},                                  // Y 5
        };
        superflat.spawn_height = superflat.flat_top_y() + 1;

        // Один биом: обычный рельеф и декорации, но без пещер и без островов.
        auto& single = table[static_cast<size_t>(Dimension_Id::Single_Biome)];
        single.id = Dimension_Id::Single_Biome;
        single.name = "single_biome";
        single.display_name = "Single biome";
        single.terrain = Terrain_Mode::Noise;
        single.caves = false;
        single.sky_islands = false;
        single.single_biome = true;

        // То же, но без земли: только парящие острова над пустотой.
        auto& islands = table[static_cast<size_t>(Dimension_Id::Single_Islands)];
        islands.id = Dimension_Id::Single_Islands;
        islands.name = "single_islands";
        islands.display_name = "Floating islands";
        islands.terrain = Terrain_Mode::Noise;
        islands.caves = false;
        islands.ground = false;
        islands.decorations = false;
        islands.sky_islands = true;
        islands.single_biome = true;

        return table;
    }

    const auto& dimension_table() {
        static const auto table = register_dimensions();
        return table;
    }
}

const Dimension_Info& get_dimension_info(Dimension_Id id) {
    const size_t index = static_cast<size_t>(id);
    return dimension_table()[index < dimension_table().size() ? index : 0];
}

Dimension_Id dimension_from_name(const std::string& name) {
    for (const Dimension_Info& info : dimension_table()) {
        if (name == info.name) return info.id;
    }
    return Dimension_Id::Overworld;
}
