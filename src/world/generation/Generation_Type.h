#ifndef OPTICRAFT_GENERATION_TYPE_H
#define OPTICRAFT_GENERATION_TYPE_H

#include <string>
#include <vector>
#include "world/generation/Biome.h"

class Generation_Type {
public:
    explicit Generation_Type(const std::string& folder_name);

    const Noise_Params& get_biome_map_noise() const { return m_biome_noise; }
    const Biome& get_biome(int biome_key) const;

private:
    enum class Compare_Mode {
        Greater = 0,
        Range = 1,
        Less = 2
    };

    struct Biome_Range {
        Biome biome;
        int min_val = 0;
        int max_val = 0;
        Compare_Mode compare = Compare_Mode::Range;

        bool matches(int biome_key) const;
    };

    std::string m_folder;
    Noise_Params m_biome_noise;
    std::vector<Biome_Range> m_biomes;

    void parse_file(const std::string& path);
};

#endif
