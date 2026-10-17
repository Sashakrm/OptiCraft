#ifndef OPTICRAFT_WORLD_FILE_H
#define OPTICRAFT_WORLD_FILE_H

#include "utils/Config.h"

#include <cstdint>
#include <filesystem>
#include <string>

#include "world/chunks/Chunk.h"
#include "world/storage/Region_File.h"

struct World_Settings {
    std::string name{"Default World"};
    std::string generation_folder{"Classic"};
    std::uint32_t seed{Config::seed};
    std::string dimension{"overworld"};  // имя из Dimension.h (get_dimension_info)
    std::string biome{"Plains"};         // имя биома (biome_name), для измерения single_biome*
    std::string game_mode{"creative"};   // "creative" | "survival"
    std::string last_played;             // "YYYY-MM-DD HH:MM", для списка миров
    bool has_player_position{false};
    float player_x{0.0f}, player_y{0.0f}, player_z{0.0f};
    float day_time{0.0f};                // нормализованное время суток [0,1)
};

class World_File {
public:
    explicit World_File(std::filesystem::path path = "worlds/default/world.json");

    bool load(World_Settings& settings) const;
    bool save(const World_Settings& settings) const;

    const std::filesystem::path& get_path() const { return m_path; }

    // Постоянное хранилище чанков: ВСЕ чанки одного пресета лежат в одном
    // region-файле (см. Region_File). Раньше на каждый чанк создавался отдельный
    // c_x_y_z.bin, и при радиусе загрузки 5 это были тысячи файлов по 32 КБ.
    std::filesystem::path get_region_path(const std::string& generation_folder) const;
    // Путь к сохранению старого формата — нужен только для миграции уже
    // существующих миров, новые файлы в этом формате не создаются.
    std::filesystem::path get_legacy_chunk_path(int chunk_x, int chunk_y, int chunk_z,
                                                const std::string& generation_folder) const;
    bool load_chunk(int chunk_x, int chunk_y, int chunk_z,
                    const std::string& generation_folder,
                    Chunk_Block_Grid& blocks) const;
    bool save_chunk(int chunk_x, int chunk_y, int chunk_z,
                    const std::string& generation_folder,
                    const Chunk_Block_Grid& blocks) const;
    // Сбрасывает индекс region-файла на диск. Звать при выгрузке мира и выходе.
    void flush_chunks(const std::string& generation_folder) const;

private:
    std::filesystem::path m_path;
};

#endif // OPTICRAFT_WORLD_FILE_H
