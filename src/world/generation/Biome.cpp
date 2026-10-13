#include "world/generation/Biome.h"

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <array>

namespace {
    std::string trim(const std::string& value) {
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(start, end - start + 1);
    }

    bool same_key(const std::string& left, const char* right) {
        return trim(left) == right;
    }

    std::string resolve_biome_path(const std::string& world_gen_folder, const std::string& name) {
        const std::array<std::string, 3> prefixes = {
            "assets/worldgen/" + world_gen_folder + "/" + name + ".biome",
            "../assets/worldgen/" + world_gen_folder + "/" + name + ".biome",
            "../../assets/worldgen/" + world_gen_folder + "/" + name + ".biome"
        };

        for (const auto& path : prefixes) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }

        return prefixes.front();
    }
}

Biome::Biome(const std::string& name, const std::string& world_gen_folder)
    : m_name(name)
{
    parse_file(resolve_biome_path(world_gen_folder, name));
}

Block_Types Biome::get_surface_block(std::mt19937& rng) const {
    if (m_surface_blocks.empty()) {
        return Block_Types::Grass;
    }
    std::uniform_int_distribution<size_t> dist(0, m_surface_blocks.size() - 1);
    return m_surface_blocks[dist(rng)];
}

Block_Types Biome::get_flora_block(std::mt19937& rng) const {
    if (m_flora_blocks.empty()) {
        return Block_Types::Tall_Grass;
    }
    std::uniform_int_distribution<size_t> dist(0, m_flora_blocks.size() - 1);
    return m_flora_blocks[dist(rng)];
}

int Biome::get_tree_type(std::mt19937& rng) const {
    if (m_tree_types.empty()) {
        return 0;
    }
    std::uniform_int_distribution<size_t> dist(0, m_tree_types.size() - 1);
    return m_tree_types[dist(rng)];
}

Structure_Type Biome::get_structure_type(std::mt19937& rng) const {
    if (m_structure_types.empty()) {
        return Structure_Type::Oak_Tree;
    }
    std::uniform_int_distribution<size_t> dist(0, m_structure_types.size() - 1);
    return m_structure_types[dist(rng)];
}

void Biome::load_block_list(std::ifstream& file, std::vector<Block_Types>& out) {
    int id = 0;
    int freq = 0;
    file >> id >> freq;

    for (int i = 0; i < freq; ++i) {
        out.push_back(static_cast<Block_Types>(id));
    }
}

void Biome::parse_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        m_surface_blocks.push_back(Block_Types::Grass);
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty()) continue;

        if (same_key(line, "Noise")) {
            file >> m_noise.octaves >> m_noise.amplitude >> m_noise.roughness
                 >> m_noise.smoothness >> m_noise.height_offset;
        } else if (same_key(line, "Surface")) {
            load_block_list(file, m_surface_blocks);
        } else if (same_key(line, "Flora")) {
            load_block_list(file, m_flora_blocks);
        } else if (same_key(line, "Tree")) {
            int id = 0;
            int freq = 0;
            file >> id >> freq;
            for (int i = 0; i < freq; ++i) {
                m_tree_types.push_back(id);
            }
        } else if (same_key(line, "Structure")) {
            int id = 0;
            int freq = 0;
            file >> id >> freq;
            for (int i = 0; i < freq; ++i) {
                m_structure_types.push_back(static_cast<Structure_Type>(id));
            }
        } else if (same_key(line, "Flora Freq")) {
            file >> m_flora_freq;
        } else if (same_key(line, "Tree Freq")) {
            file >> m_tree_freq;
        } else if (same_key(line, "Structure Freq")) {
            file >> m_structure_freq;
        }
    }
}
