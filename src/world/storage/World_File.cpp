#include "world/storage/World_File.h"

#include "utils/logging/Logger.h"

#include <charconv>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <array>
#include <cstring>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace {
    void skip_whitespace(const std::string& input, size_t& position) {
        while (position < input.size() && std::isspace(static_cast<unsigned char>(input[position]))) {
            ++position;
        }
    }

    std::optional<std::string> read_string_field(const std::string& input, const std::string& key) {
        const std::string marker = "\"" + key + "\"";
        size_t position = input.find(marker);
        if (position == std::string::npos) {
            return std::nullopt;
        }

        position = input.find(':', position + marker.size());
        if (position == std::string::npos) {
            return std::nullopt;
        }
        ++position;
        skip_whitespace(input, position);
        if (position >= input.size() || input[position] != '\"') {
            return std::nullopt;
        }
        ++position;

        std::string value;
        while (position < input.size()) {
            const char character = input[position++];
            if (character == '\"') {
                return value;
            }
            if (character != '\\' || position >= input.size()) {
                value += character;
                continue;
            }

            const char escaped = input[position++];
            switch (escaped) {
                case '\"': value += '\"'; break;
                case '\\': value += '\\'; break;
                case 'n': value += '\n'; break;
                default: return std::nullopt;
            }
        }

        return std::nullopt;
    }

    std::optional<std::uint32_t> read_uint32_field(const std::string& input, const std::string& key) {
        const std::string marker = "\"" + key + "\"";
        size_t position = input.find(marker);
        if (position == std::string::npos) {
            return std::nullopt;
        }

        position = input.find(':', position + marker.size());
        if (position == std::string::npos) {
            return std::nullopt;
        }
        ++position;
        skip_whitespace(input, position);

        std::uint32_t value = 0;
        const char* first = input.data() + position;
        const char* last = input.data() + input.size();
        const auto [end, error] = std::from_chars(first, last, value);
        if (error != std::errc{} || end == first) {
            return std::nullopt;
        }
        size_t end_position = static_cast<size_t>(end - input.data());
        skip_whitespace(input, end_position);
        if (end_position < input.size() && input[end_position] != ',' && input[end_position] != '}') {
            return std::nullopt;
        }
        return value;
    }

    std::optional<float> read_float_field(const std::string& input, const std::string& key) {
        const std::string marker = "\"" + key + "\"";
        size_t position = input.find(marker);
        if (position == std::string::npos) return std::nullopt;
        position = input.find(':', position + marker.size());
        if (position == std::string::npos) return std::nullopt;
        ++position;
        skip_whitespace(input, position);
        char* end = nullptr;
        const float value = std::strtof(input.c_str() + position, &end);
        if (end == input.c_str() + position) return std::nullopt;
        return value;
    }

    std::string escape_json(const std::string& value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char character : value) {
            switch (character) {
                case '\"': escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n"; break;
                default: escaped += character; break;
            }
        }
        return escaped;
    }
}

World_File::World_File(std::filesystem::path path)
    : m_path(std::move(path))
{}

bool World_File::load(World_Settings& settings) const {
    std::ifstream file(m_path);
    if (!file) {
        return false;
    }

    const std::string contents{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    };

    bool parsed_any_field = false;
    if (const auto name = read_string_field(contents, "name")) {
        settings.name = *name;
        parsed_any_field = true;
    }
    if (const auto folder = read_string_field(contents, "generation_folder")) {
        settings.generation_folder = *folder;
        parsed_any_field = true;
    }
    if (const auto seed = read_uint32_field(contents, "seed")) {
        settings.seed = *seed;
        parsed_any_field = true;
    }

    if (const auto mode = read_string_field(contents, "game_mode")) settings.game_mode = *mode;
    if (const auto played = read_string_field(contents, "last_played")) settings.last_played = *played;
    const auto px = read_float_field(contents, "player_x");
    const auto py = read_float_field(contents, "player_y");
    const auto pz = read_float_field(contents, "player_z");
    settings.has_player_position = px && py && pz;
    if (settings.has_player_position) { settings.player_x = *px; settings.player_y = *py; settings.player_z = *pz; }
    if (const auto dt = read_float_field(contents, "day_time")) settings.day_time = *dt;

    if (!parsed_any_field) {
        LOG_WARN("World settings file is invalid: " + m_path.string());
    }
    return parsed_any_field;
}

bool World_File::save(const World_Settings& settings) const {
    std::error_code error;
    const std::filesystem::path directory = m_path.parent_path();
    if (!directory.empty()) {
        std::filesystem::create_directories(directory, error);
        if (error) {
            LOG_ERROR("Cannot create world directory " + directory.string() + ": " + error.message());
            return false;
        }
    }

    std::ofstream file(m_path, std::ios::trunc);
    if (!file) {
        LOG_ERROR("Cannot write world settings: " + m_path.string());
        return false;
    }

    file << "{\n"
         << "  \"name\": \"" << escape_json(settings.name) << "\",\n"
         << "  \"generation_folder\": \"" << escape_json(settings.generation_folder) << "\",\n"
         << "  \"seed\": " << settings.seed << ",\n"
         << "  \"game_mode\": \"" << escape_json(settings.game_mode) << "\",\n"
         << "  \"last_played\": \"" << escape_json(settings.last_played) << "\",\n";
    if (settings.has_player_position) {
        file << "  \"player_x\": " << settings.player_x << ",\n"
             << "  \"player_y\": " << settings.player_y << ",\n"
             << "  \"player_z\": " << settings.player_z << ",\n";
    }
    file << "  \"day_time\": " << settings.day_time << "\n"
         << "}\n";

    if (!file) {
        LOG_ERROR("Failed while writing world settings: " + m_path.string());
        return false;
    }
    return true;
}


std::filesystem::path World_File::get_region_path(const std::string& generation_folder) const {
    // Разные пресеты генерации остаются изолированными: F6 переключает Classic и
    // Islands при одном и том же world.json, и смешивать их чанки нельзя.
    const std::filesystem::path world_root = m_path.parent_path();
    return world_root / "chunks" / (generation_folder + ".region");
}

std::filesystem::path World_File::get_legacy_chunk_path(int chunk_x, int chunk_y, int chunk_z,
                                                        const std::string& generation_folder) const {
    const std::filesystem::path world_root = m_path.parent_path();
    return world_root / "chunks" / generation_folder /
           ("c_" + std::to_string(chunk_x) + "_" + std::to_string(chunk_y) + "_" +
            std::to_string(chunk_z) + ".bin");
}

namespace {
    // Чтение чанка в старом формате (по файлу на чанк). Оставлено только для
    // миграции: уже существующие миры не должны потеряться при обновлении.
    bool read_legacy_chunk(const std::filesystem::path& path, Chunk_Block_Grid& blocks) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;

        char magic[4]{};
        std::uint32_t version = 0;
        std::uint32_t size_x = 0, size_y = 0, size_z = 0;
        file.read(magic, sizeof(magic));
        file.read(reinterpret_cast<char*>(&version), sizeof(version));
        file.read(reinterpret_cast<char*>(&size_x), sizeof(size_x));
        file.read(reinterpret_cast<char*>(&size_y), sizeof(size_y));
        file.read(reinterpret_cast<char*>(&size_z), sizeof(size_z));

        if (!file || std::memcmp(magic, "OPC1", 4) != 0 ||
            (version != 1 && version != 2) ||
            size_x != Config::chunk_size || size_y != Config::chunk_height ||
            size_z != Config::chunk_size) {
            return false;
        }

        if (version == 2) {
            std::vector<std::uint8_t> payload(blocks.size_bytes());
            file.read(reinterpret_cast<char*>(payload.data()),
                      static_cast<std::streamsize>(payload.size()));
            if (!file) return false;
            for (auto id : payload) {
                if (id >= k_max_block_types) return false;
            }
            std::memcpy(blocks.data(), payload.data(), payload.size());
            return true;
        }

        for (int y = 0; y < Config::chunk_height; ++y) {
            for (int z = 0; z < Config::chunk_size; ++z) {
                for (int x = 0; x < Config::chunk_size; ++x) {
                    std::uint16_t legacy_id = 0;
                    file.read(reinterpret_cast<char*>(&legacy_id), sizeof(legacy_id));
                    if (!file || legacy_id >= k_max_block_types) return false;
                    blocks.set(x, y, z, static_cast<Block_Types>(legacy_id));
                }
            }
        }
        return true;
    }
}

bool World_File::load_chunk(int chunk_x, int chunk_y, int chunk_z,
                            const std::string& generation_folder,
                            Chunk_Block_Grid& blocks) const {
    auto region = Region_File::get(get_region_path(generation_folder));
    if (!region) return false;

    const Region_File::Key key{chunk_x, chunk_y, chunk_z};
    if (region->read(key, blocks)) return true;

    // Миграция: чанка нет в region-файле, но может лежать старый c_x_y_z.bin.
    // Переносим его внутрь region-файла и удаляем исходник, чтобы каталог
    // постепенно опустел без отдельного шага конвертации мира.
    const auto legacy_path = get_legacy_chunk_path(chunk_x, chunk_y, chunk_z, generation_folder);
    std::error_code error;
    if (!std::filesystem::exists(legacy_path, error) || error) return false;

    if (!read_legacy_chunk(legacy_path, blocks)) {
        LOG_WARN("Ignoring incompatible chunk save: " + legacy_path.string());
        return false;
    }

    if (region->write(key, blocks)) {
        error.clear();
        std::filesystem::remove(legacy_path, error);
    }
    return true;
}

bool World_File::save_chunk(int chunk_x, int chunk_y, int chunk_z,
                            const std::string& generation_folder,
                            const Chunk_Block_Grid& blocks) const {
    auto region = Region_File::get(get_region_path(generation_folder));
    if (!region) return false;
    return region->write(Region_File::Key{chunk_x, chunk_y, chunk_z}, blocks);
}

void World_File::flush_chunks(const std::string& generation_folder) const {
    auto region = Region_File::get(get_region_path(generation_folder));
    if (region) region->flush();
}
