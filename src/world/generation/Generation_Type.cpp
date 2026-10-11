#include "world/generation/Generation_Type.h"

#include <array>
#include <fstream>
#include <filesystem>
#include <stdexcept>

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

    std::string resolve_info_path(const std::string& folder) {
        const std::array<std::string, 3> prefixes = {
            "assets/worldgen/" + folder + "/Info.biome",
            "../assets/worldgen/" + folder + "/Info.biome",
            "../../assets/worldgen/" + folder + "/Info.biome"
        };

        for (const auto& path : prefixes) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }

        return prefixes.front();
    }
}

bool Generation_Type::Biome_Range::matches(int biome_key) const {
    switch (compare) {
        case Compare_Mode::Greater:
            return biome_key >= max_val;
        case Compare_Mode::Less:
            return biome_key <= max_val;
        case Compare_Mode::Range:
            return biome_key >= min_val && biome_key <= max_val;
    }
    return false;
}

Generation_Type::Generation_Type(const std::string& folder_name)
    : m_folder(folder_name)
{
    parse_file(resolve_info_path(folder_name));

    if (m_biomes.empty()) {
        m_biomes.push_back({Biome("Grassland", folder_name), 0, 255, Compare_Mode::Range});
    }
}

const Biome& Generation_Type::get_biome(int biome_key) const {
    for (const auto& entry : m_biomes) {
        if (entry.matches(biome_key)) {
            return entry.biome;
        }
    }

    return m_biomes.front().biome;
}

void Generation_Type::parse_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty()) continue;

        if (same_key(line, "Noise")) {
            file >> m_biome_noise.octaves >> m_biome_noise.amplitude >> m_biome_noise.roughness
                 >> m_biome_noise.smoothness >> m_biome_noise.height_offset;
        } else if (same_key(line, "Biome")) {
            std::string biome_name;
            std::string compare_string;
            int min_val = 0;
            int max_val = 0;

            std::getline(file, biome_name);
            std::getline(file, compare_string);
            biome_name = trim(biome_name);
            compare_string = trim(compare_string);

            Compare_Mode compare_mode = Compare_Mode::Range;

            if (compare_string == "Greater") {
                compare_mode = Compare_Mode::Greater;
                file >> max_val;
            } else if (compare_string == "Less") {
                compare_mode = Compare_Mode::Less;
                file >> max_val;
            } else {
                compare_mode = Compare_Mode::Range;
                file >> min_val >> max_val;
            }

            m_biomes.push_back({
                Biome(biome_name, m_folder),
                min_val,
                max_val,
                compare_mode
            });
        }
    }
}
