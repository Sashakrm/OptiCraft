#ifndef OPTICRAFT_BIOME_H
#define OPTICRAFT_BIOME_H

#include <string>
#include <vector>
#include <random>
#include "world/blocks/Block_Types.h"
#include "world/generation/Structures.h"

struct Noise_Params {
    int octaves = 4;
    int amplitude = 85;
    float roughness = 0.5f;
    float smoothness = 235.0f;
    int height_offset = 0;
};

class Biome {
public:
    Biome(const std::string& name, const std::string& world_gen_folder);

    Block_Types get_surface_block(std::mt19937& rng) const;
    Block_Types get_flora_block(std::mt19937& rng) const;
    int get_tree_type(std::mt19937& rng) const;
    Structure_Type get_structure_type(std::mt19937& rng) const;

    bool has_flora() const { return !m_flora_blocks.empty(); }
    bool has_trees() const { return !m_tree_types.empty(); }
    bool has_structures() const { return !m_structure_types.empty(); }
    int get_flora_frequency() const { return m_flora_freq; }
    int get_tree_frequency() const { return m_tree_freq; }
    int get_structure_frequency() const { return m_structure_freq; }
    const Noise_Params& get_noise() const { return m_noise; }

private:
    std::string m_name;

    Noise_Params m_noise;
    std::vector<Block_Types> m_surface_blocks;
    std::vector<Block_Types> m_flora_blocks;
    std::vector<int> m_tree_types;
    std::vector<Structure_Type> m_structure_types;
    int m_flora_freq = 0;
    int m_tree_freq = 0;
    int m_structure_freq = 0;

    void parse_file(const std::string& path);
    void load_block_list(std::ifstream& file, std::vector<Block_Types>& out);
};

#endif
