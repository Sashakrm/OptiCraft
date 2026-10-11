//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_CHUNK_H
#define OPTICRAFT_CHUNK_H

#include <array>
#include <cstdint>
#include <vector>
#include <functional>
#include <optional>
#include <unordered_map>
#include <glm/glm.hpp>
#include "world/blocks/Block_Types.h"
#include "utils/Config.h"

struct Chunk_Vertex {
    float x, y, z;
    float u, v;
    float nx, ny, nz;
    float light;
};

struct Chunk_Section_Range {
    size_t solid_start = 0, solid_count = 0;
    size_t flora_start = 0, flora_count = 0;
    size_t liquid_start = 0, liquid_count = 0;
    float min_y = 0.0f;
    float max_y = 0.0f;
    bool has_geometry = false;
};

struct Chunk_Meshes {
    std::vector<Chunk_Vertex> solid;
    std::vector<Chunk_Vertex> flora;
    std::vector<Chunk_Vertex> liquid;
    std::array<Chunk_Section_Range, Config::chunk_section_count> sections;
};

class Chunk_Manager;
class World_Generator;
class Texture_Atlas;

// A chunk is deliberately stored as a flat, cache-friendly byte array.
// 32*32*32 = 32768 bytes = exactly 32 KiB for block IDs.
// Block_ID is uint16_t for the public registry, but the registry is limited to 256 IDs,
// so the persistent/runtime representation only needs one byte.
class Chunk_Block_Grid {
public:
    static constexpr size_t voxel_count = static_cast<size_t>(Config::chunk_size) *
                                           Config::chunk_height * Config::chunk_size;

private:
    std::array<uint8_t, voxel_count> m_data{};

    // Метаданные блока (см. Block_Meta в Block_Types.h): ориентация бревна, сторона, куда
    // смотрит печь, флаг "активен". Хранятся РЕДКО — почти все блоки мира имеют meta = 0, —
    // поэтому это разреженная карта индекс -> значение, а не второй массив на 32 КБ.
    // Живёт внутри сетки блоков, чтобы автоматически копироваться в снимок для мешинга
    // (Mesh_Job::own_blocks) и попадать в Region_File вместе с блоками.
    std::unordered_map<uint16_t, uint8_t> m_meta;

    static constexpr size_t index(int x, int y, int z) {
        return (static_cast<size_t>(y) * Config::chunk_size + static_cast<size_t>(z)) *
               Config::chunk_size + static_cast<size_t>(x);
    }

public:
    Block_Types get(int x, int y, int z) const {
        return static_cast<Block_Types>(m_data[index(x, y, z)]);
    }

    // Замена блока сбрасывает его метаданные: новый блок не должен унаследовать
    // ориентацию/активность от прежнего.
    void set(int x, int y, int z, Block_Types type) {
        const size_t i = index(x, y, z);
        m_data[i] = static_cast<uint8_t>(static_cast<Block_ID>(type));
        if (!m_meta.empty()) m_meta.erase(static_cast<uint16_t>(i));
    }

    void fill(Block_Types type) {
        m_data.fill(static_cast<uint8_t>(static_cast<Block_ID>(type)));
        m_meta.clear();
    }

    uint8_t get_meta(int x, int y, int z) const {
        if (m_meta.empty()) return 0;
        const auto it = m_meta.find(static_cast<uint16_t>(index(x, y, z)));
        return it == m_meta.end() ? 0 : it->second;
    }

    void set_meta(int x, int y, int z, uint8_t value) {
        const uint16_t i = static_cast<uint16_t>(index(x, y, z));
        if (value == 0) m_meta.erase(i);
        else m_meta[i] = value;
    }

    bool has_meta() const { return !m_meta.empty(); }
    void clear_meta() { m_meta.clear(); }
    const std::unordered_map<uint16_t, uint8_t>& meta_entries() const { return m_meta; }
    // Для загрузки с диска: index — линейный индекс вокселя (как в index()).
    void set_meta_by_index(uint16_t linear_index, uint8_t value) {
        if (linear_index < voxel_count && value != 0) m_meta[linear_index] = value;
    }

    const uint8_t* data() const { return m_data.data(); }
    uint8_t* data() { return m_data.data(); }
    constexpr size_t size_bytes() const { return voxel_count; }

    // Keeps copy semantics cheap and explicit for async mesh jobs.
    Chunk_Block_Grid() = default;
};

// Each voxel stores BOTH Minecraft-style block light and sky light in one byte:
// low nibble = block light (0..14), high nibble = sky light (0..15).
// One byte per voxel -> 32768 bytes = 32 KiB, same footprint as Chunk_Block_Grid.
// (An earlier version tried to save memory by packing two VOXELS into one byte
// instead — that only leaves room for a single 4-bit value per voxel, so it could
// not actually hold two independent channels: get_sky() ended up right-shifting an
// already-4-bit value and always returned 0, and set_block()/set_sky() both wrote
// the same shared nibble and silently clobbered each other.)
class Chunk_Light_Grid {
public:
    static constexpr size_t voxel_count = Chunk_Block_Grid::voxel_count;
    static constexpr size_t byte_count = voxel_count;

private:
    std::array<uint8_t, byte_count> m_data{};

    static constexpr size_t index(int x, int y, int z) {
        return (static_cast<size_t>(y) * Config::chunk_size + static_cast<size_t>(z)) *
               Config::chunk_size + static_cast<size_t>(x);
    }

public:
    // Full packed byte for this voxel: low nibble = block light, high nibble = sky light.
    uint8_t get(int x, int y, int z) const {
        return m_data[index(x, y, z)];
    }

    uint8_t get_block(int x, int y, int z) const {
        return static_cast<uint8_t>(m_data[index(x, y, z)] & 0x0Fu);
    }

    uint8_t get_sky(int x, int y, int z) const {
        return static_cast<uint8_t>(m_data[index(x, y, z)] >> 4);
    }

    void set_block(int x, int y, int z, uint8_t level) {
        uint8_t& packed = m_data[index(x, y, z)];
        packed = static_cast<uint8_t>((packed & 0xF0u) | (level & 0x0Fu));
    }

    void set_sky(int x, int y, int z, uint8_t level) {
        uint8_t& packed = m_data[index(x, y, z)];
        packed = static_cast<uint8_t>((packed & 0x0Fu) | ((level & 0x0Fu) << 4));
    }

    void clear_block() {
        for (auto& b : m_data) b &= 0xF0u;
    }

    void clear_sky() {
        for (auto& b : m_data) b &= 0x0Fu;
    }

    void fill(uint8_t level) {
        level &= 0x0Fu;
        m_data.fill(static_cast<uint8_t>(level | (level << 4)));
    }

    const uint8_t* data() const { return m_data.data(); }
    uint8_t* data() { return m_data.data(); }
    constexpr size_t size_bytes() const { return byte_count; }
};

static_assert(Chunk_Block_Grid::voxel_count == 32 * 32 * 32, "Chunk block storage must stay 32 KiB");
static_assert(sizeof(Chunk_Light_Grid) == 32 * 32 * 32, "Chunk light storage must stay 32 KiB");

using Chunk_Neighbor_Lookup = std::function<Block_Types(int world_x, int world_y, int world_z)>;

// Mesh jobs only need one-cell borders from neighboring chunks. Copying six complete
// 48 KiB chunks was needlessly expensive (336 KiB/job). A face snapshot is only 32*32
// bytes for blocks or 32*32 bytes for light (one packed block+sky byte per voxel,
// same convention as Chunk_Light_Grid).
struct Chunk_Block_Face {
    std::array<uint8_t, static_cast<size_t>(Config::chunk_size) * Config::chunk_height> data{};

    Block_Types get(int a, int b) const {
        return static_cast<Block_Types>(data[static_cast<size_t>(b) * Config::chunk_size + a]);
    }
};

struct Chunk_Light_Face {
    static constexpr size_t voxel_count = static_cast<size_t>(Config::chunk_size) * Config::chunk_height;
    std::array<uint8_t, voxel_count> data{};

    uint8_t get(int a, int b) const {
        return data[static_cast<size_t>(b) * Config::chunk_size + a];
    }
};

using Chunk_Block_Faces = std::array<std::optional<Chunk_Block_Face>, 6>;
using Chunk_Light_Faces = std::array<std::optional<Chunk_Light_Face>, 6>;
using Chunk_Light_Lookup = std::function<uint8_t(int world_x, int world_y, int world_z)>;

Chunk_Meshes build_chunk_mesh(const Chunk_Block_Grid& blocks,
                              const Chunk_Light_Grid& light,
                              int chunk_x, int chunk_y, int chunk_z,
                              const Chunk_Neighbor_Lookup& neighbor_lookup,
                              const Chunk_Light_Lookup& light_lookup,
                              const Texture_Atlas& atlas,
                              bool only_positive_faces = false);

class Chunk {
private:
    int m_chunk_x;
    int m_chunk_y;
    int m_chunk_z;

    Chunk_Block_Grid m_blocks;
    Chunk_Light_Grid m_block_light; // packed: low nibble block light, high nibble sky light
    std::array<int, Config::chunk_height> m_layer_block_count{};

    const World_Generator* m_world_generator = nullptr;
    const Chunk_Manager* m_chunk_manager = nullptr;
    const Texture_Atlas* m_texture_atlas = nullptr;

    Chunk_Meshes m_meshes;
    bool m_is_mesh_dirty = true;
    // Отличает чанк, который игрок действительно правил, от нетронутого.
    // Нетронутый чанк полностью восстанавливается генератором по сиду, поэтому
    // писать его на диск бессмысленно: раньше выгрузка сохраняла ВСЕ чанки
    // подряд, то есть тысячи записей по 32 КБ при обычной ходьбе по миру.
    bool m_is_modified = false;
    uint64_t m_mesh_version = 0;

public:
    Chunk(int chunk_x, int chunk_y, int chunk_z, const World_Generator& generator);

    void set_chunk_manager(const Chunk_Manager* manager) { m_chunk_manager = manager; }
    void set_texture_atlas(const Texture_Atlas* atlas) { m_texture_atlas = atlas; }

    void generate_blocks();
    void recalculate_metadata();
    void build_mesh();
    void apply_mesh(Chunk_Meshes meshes);

    Block_Types get_block(int x, int y, int z) const;
    void set_block(int x, int y, int z, Block_Types type);
    uint8_t get_meta(int x, int y, int z) const;
    // Меняет только метаданные (блок остаётся тем же): помечает чанк изменённым и
    // требующим перестройки меша.
    void set_meta(int x, int y, int z, uint8_t value);
    const Chunk_Block_Grid& get_blocks() const { return m_blocks; }
    Chunk_Block_Grid& get_blocks_mutable() { return m_blocks; }

    uint8_t get_light(int x, int y, int z) const;
    void set_light(int x, int y, int z, uint8_t level);
    uint8_t get_sky_light(int x, int y, int z) const { return m_block_light.get_sky(x, y, z); }
    void set_sky_light(int x, int y, int z, uint8_t level) { m_block_light.set_sky(x, y, z, level); }
    uint8_t get_packed_light(int x, int y, int z) const { return m_block_light.get(x, y, z); }
    void clear_light() { m_block_light.fill(0); }
    void clear_block_light() { m_block_light.clear_block(); }
    void clear_sky_light() { m_block_light.clear_sky(); }
    const Chunk_Light_Grid& get_light_grid() const { return m_block_light; }

    int get_chunk_x() const { return m_chunk_x; }
    int get_chunk_y() const { return m_chunk_y; }
    int get_chunk_z() const { return m_chunk_z; }
    const Chunk_Meshes& get_meshes() const { return m_meshes; }
    uint64_t get_mesh_version() const { return m_mesh_version; }
    bool is_mesh_dirty() const { return m_is_mesh_dirty; }
    bool is_modified() const { return m_is_modified; }
    void mark_modified() { m_is_modified = true; }
    void clear_modified() { m_is_modified = false; }
    void mark_mesh_dirty() { m_is_mesh_dirty = true; }
    bool is_empty() const;
    bool has_blocks() const {
        for (const int count : m_layer_block_count) {
            if (count != 0) return true;
        }
        return false;
    }

    glm::vec3 get_world_position(int local_x, int local_y, int local_z) const;
};

#endif // OPTICRAFT_CHUNK_H
