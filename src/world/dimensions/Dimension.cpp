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
        empty.decorations = false;
        empty.sky_islands = false;
        empty.spawn_height = 64;

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
