//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_CHUNK_H
#define OPTICRAFT_CHUNK_H

#include <array>
#include <cstdint>
#include <cstring>
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

// Вершина непрозрачной геометрии чанка (жадный меш). Один квад может покрывать целый
// прямоугольник одинаковых граней (например 12x5 блоков камня), поэтому UV здесь — не готовые
// текстурные координаты, а ЛОКАЛЬНЫЕ координаты в блоках (0..w, 0..h), а прямоугольник спрайта
// в атласе передаётся отдельно (rect = u_min, v_min, u_max, v_max). Фрагментный шейдер делает
// uv = mix(rect.xy, rect.zw, fract(local)) — текстура блока повторяется внутри слитого квада.
// Нормаль сведена к ny: шейдер смотрит только на её Y (верх / низ / бок).
struct Terrain_Vertex {
    float x, y, z;
    float local_u, local_v;
    float ny;
    float light;
    float rect_u_min, rect_v_min, rect_u_max, rect_v_max;
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
    std::vector<Terrain_Vertex> solid;
    std::vector<Chunk_Vertex> flora;
    std::vector<Chunk_Vertex> liquid;
    std::array<Chunk_Section_Range, Config::chunk_section_count> sections;
};

class Chunk_Manager;
class World_Generator;
class Texture_Atlas;

// Блоки чанка хранятся палитрой, как в Minecraft: у чанка есть небольшой список ТОЛЬКО тех
// блоков, которые в нём реально встречаются (палитра), а каждый воксель хранит не id блока, а
// номер в этом списке, упакованный в 1/2/4/8 бит:
//
//   блоков в чанке   бит на воксель   размер данных (32x32x32)
//   1 (воздух/камень)       0                 0 байт (данных нет вообще)
//   2                       1                 4 КБ
//   3..4                    2                 8 КБ
//   5..16                   4                16 КБ
//   17..256                 8                32 КБ   (как было раньше у ВСЕХ чанков)
//
// Ширины подобраны степенями двойки, чтобы чтение воксела сводилось к сдвигам и маскам без
// деления — get() вызывается в самых горячих циклах (мешинг, освещение).
// Id блока по-прежнему один байт (реестр ограничен 256 id, см. k_max_block_types).
class Chunk_Block_Grid {
public:
    static constexpr size_t voxel_count = static_cast<size_t>(Config::chunk_size) *
                                           Config::chunk_height * Config::chunk_size;

private:
    std::vector<uint8_t> m_palette;   // номер в палитре -> id блока (всегда минимум 1 запись)
    std::vector<uint16_t> m_counts;   // сколько вокселей использует каждая запись палитры
    std::vector<uint64_t> m_words;    // упакованные номера; пусто, когда m_bits == 0
    uint8_t m_bits = 0;               // 0, 1, 2, 4 или 8
    uint8_t m_log_bits = 0;           // log2(m_bits)
    uint8_t m_word_shift = 0;         // log2(вокселей в 64-битном слове)
    uint8_t m_slot_mask = 0;          // (вокселей в слове) - 1
    uint8_t m_value_mask = 0;         // (1 << m_bits) - 1

    // Метаданные блока (см. Block_Meta в Block_Types.h): ориентация бревна, сторона, куда
    // смотрит печь, флаг "активен". Хранятся РЕДКО — почти все блоки мира имеют meta = 0, —
    // поэтому это разреженная карта индекс -> значение, а не второй массив.
    // Живёт внутри сетки блоков, чтобы автоматически копироваться в снимок для мешинга
    // (Mesh_Job::own_blocks) и попадать в Region_File вместе с блоками.
    std::unordered_map<uint16_t, uint8_t> m_meta;

    static constexpr size_t index(int x, int y, int z) {
        return (static_cast<size_t>(y) * Config::chunk_size + static_cast<size_t>(z)) *
               Config::chunk_size + static_cast<size_t>(x);
    }

    static constexpr uint8_t bits_for_palette(size_t palette_size) {
        return palette_size <= 1 ? 0 : palette_size <= 2 ? 1 : palette_size <= 4 ? 2
             : palette_size <= 16 ? 4 : 8;
    }
    static constexpr size_t capacity_for_bits(uint8_t bits) {
        return bits == 0 ? 1 : (static_cast<size_t>(1) << bits);
    }

    void apply_bits(uint8_t bits);
    // Перепаковывает номера под новую ширину (remap — необязательная таблица старый -> новый номер).
    void repack(uint8_t new_bits, const uint8_t* remap = nullptr);

    uint8_t index_at(size_t i) const {
        if (m_bits == 0) return 0;
        const uint64_t word = m_words[i >> m_word_shift];
        const unsigned offset = (static_cast<unsigned>(i) & m_slot_mask) << m_log_bits;
        return static_cast<uint8_t>((word >> offset) & m_value_mask);
    }
    void write_index(size_t i, uint8_t palette_index) {
        uint64_t& word = m_words[i >> m_word_shift];
        const unsigned offset = (static_cast<unsigned>(i) & m_slot_mask) << m_log_bits;
        word = (word & ~(static_cast<uint64_t>(m_value_mask) << offset)) |
               (static_cast<uint64_t>(palette_index) << offset);
    }
    // Номер записи палитры для id блока; добавляет запись (при необходимости расширяя битность).
    uint8_t palette_index_for(uint8_t block_id);

public:
    Chunk_Block_Grid() { m_palette.assign(1, 0); m_counts.assign(1, static_cast<uint16_t>(voxel_count)); }

    Block_Types get(int x, int y, int z) const {
        return static_cast<Block_Types>(m_palette[index_at(index(x, y, z))]);
    }

    // Замена блока сбрасывает его метаданные: новый блок не должен унаследовать
    // ориентацию/активность от прежнего.
    void set(int x, int y, int z, Block_Types type) {
        const size_t i = index(x, y, z);
        if (!m_meta.empty()) m_meta.erase(static_cast<uint16_t>(i));
        const uint8_t id = static_cast<uint8_t>(static_cast<Block_ID>(type));
        const uint8_t old_index = index_at(i);
        if (m_palette[old_index] == id) return;
        const uint8_t new_index = palette_index_for(id);   // может перепаковать m_words
        // old_index не меняется при перепаковке "на месте" (записи не переупорядочиваются)
        --m_counts[old_index];
        ++m_counts[new_index];
        write_index(i, new_index);
    }

    void fill(Block_Types type) {
        m_palette.assign(1, static_cast<uint8_t>(static_cast<Block_ID>(type)));
        m_counts.assign(1, static_cast<uint16_t>(voxel_count));
        m_words.clear();
        m_words.shrink_to_fit();
        apply_bits(0);
        m_meta.clear();
    }

    // Загрузка/сохранение целиком "плоским" массивом id (voxel_count байт, порядок y,z,x).
    // Строит палитру за один проход — намного быстрее, чем voxel_count вызовов set().
    void assign_flat(const uint8_t* flat);
    void unpack(uint8_t* out) const;
    // Выкидывает записи палитры, которые больше никто не использует, и ужимает битность.
    // Зовётся после генерации и перед сохранением: set() только добавляет записи.
    void compact();

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

    // Весь чанк состоит из одного блока (после compact()): воздух над землёй, сплошной камень.
    bool is_uniform() const { return m_palette.size() == 1 || m_bits == 0; }
    Block_Types uniform_block() const { return static_cast<Block_Types>(m_palette[0]); }
    // Сколько записей в палитре и сколько байт реально занимают данные чанка (для отладки/F3).
    size_t palette_size() const { return m_palette.size(); }
    size_t memory_bytes() const {
        return m_words.size() * sizeof(uint64_t) + m_palette.size() +
               m_counts.size() * sizeof(uint16_t) +
               m_meta.size() * (sizeof(uint16_t) + sizeof(uint8_t));
    }
    constexpr size_t size_bytes() const { return voxel_count; }  // размер распакованного вида
};

// Each voxel stores BOTH Minecraft-style block light and sky light in one byte:
// low nibble = block light (0..14), high nibble = sky light (0..15).
//
// Свет чанка часто одинаков во всех вокселях (чанк целиком под открытым небом = 0xF0,
// чанк в глухой породе = 0x00), поэтому массив на 32 КБ создаётся лениво: пока все воксели
// равны, хранится ОДНО значение. Первая запись, которая отличается, разворачивает массив;
// compact() сворачивает обратно, когда всё снова выровнялось.
class Chunk_Light_Grid {
public:
    static constexpr size_t voxel_count = Chunk_Block_Grid::voxel_count;
    static constexpr size_t byte_count = voxel_count;

private:
    uint8_t m_uniform = 0;            // значение всех вокселей, пока m_data пуст
    std::vector<uint8_t> m_data;      // пуст = "все воксели == m_uniform"

    static constexpr size_t index(int x, int y, int z) {
        return (static_cast<size_t>(y) * Config::chunk_size + static_cast<size_t>(z)) *
               Config::chunk_size + static_cast<size_t>(x);
    }

    void write(size_t i, uint8_t value) {
        if (m_data.empty()) {
            if (value == m_uniform) return;
            m_data.assign(byte_count, m_uniform);
        }
        m_data[i] = value;
    }

public:
    // Full packed byte for this voxel: low nibble = block light, high nibble = sky light.
    uint8_t get(int x, int y, int z) const {
        return m_data.empty() ? m_uniform : m_data[index(x, y, z)];
    }

    uint8_t get_block(int x, int y, int z) const {
        return static_cast<uint8_t>(get(x, y, z) & 0x0Fu);
    }

    uint8_t get_sky(int x, int y, int z) const {
        return static_cast<uint8_t>(get(x, y, z) >> 4);
    }

    void set_block(int x, int y, int z, uint8_t level) {
        const uint8_t packed = get(x, y, z);
        write(index(x, y, z), static_cast<uint8_t>((packed & 0xF0u) | (level & 0x0Fu)));
    }

    void set_sky(int x, int y, int z, uint8_t level) {
        const uint8_t packed = get(x, y, z);
        write(index(x, y, z), static_cast<uint8_t>((packed & 0x0Fu) | ((level & 0x0Fu) << 4)));
    }

    void clear_block() {
        if (m_data.empty()) { m_uniform &= 0xF0u; return; }
        for (auto& b : m_data) b &= 0xF0u;
    }

    void clear_sky() {
        if (m_data.empty()) { m_uniform &= 0x0Fu; return; }
        for (auto& b : m_data) b &= 0x0Fu;
    }

    void fill(uint8_t level) {
        level &= 0x0Fu;
        m_uniform = static_cast<uint8_t>(level | (level << 4));
        m_data.clear();
        m_data.shrink_to_fit();
    }

    // Сворачивает массив обратно в одно значение, если все воксели равны.
    void compact() {
        if (m_data.empty()) return;
        const uint8_t first = m_data[0];
        for (const uint8_t b : m_data) {
            if (b != first) return;
        }
        m_uniform = first;
        m_data.clear();
        m_data.shrink_to_fit();
    }

    // Плоская копия (voxel_count байт, порядок y,z,x) — для мешера.
    void unpack(uint8_t* out) const {
        if (m_data.empty()) std::memset(out, m_uniform, byte_count);
        else std::memcpy(out, m_data.data(), byte_count);
    }

    bool is_uniform() const { return m_data.empty(); }
    size_t memory_bytes() const { return m_data.size() + 1; }
    constexpr size_t size_bytes() const { return byte_count; }
};

using Chunk_Neighbor_Lookup = std::function<Block_Types(int world_x, int world_y, int world_z)>;

// Mesh jobs only need one-cell borders from neighboring chunks. Copying six complete
// 48 KiB chunks was needlessly expensive (336 KiB/job). A face snapshot is only 32*32
// bytes for blocks or 32*32 bytes for light (one packed block+sky byte per voxel,
// same convention as Chunk_Light_Grid).
struct Chunk_Block_Face {
    std::array<uint8_t, static_cast<size_t>(Config::chunk_size) * Config::chunk_height> data{};
    // Метаданные блоков той же грани: нужны мешеру, чтобы знать уровень жидкости у соседа.
    std::array<uint8_t, static_cast<size_t>(Config::chunk_size) * Config::chunk_height> meta{};

    Block_Types get(int a, int b) const {
        return static_cast<Block_Types>(data[static_cast<size_t>(b) * Config::chunk_size + a]);
    }
    uint8_t get_meta(int a, int b) const {
        return meta[static_cast<size_t>(b) * Config::chunk_size + a];
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
// Метаданные блока за границей чанка (уровень жидкости соседа). nullptr/пусто = 0.
using Chunk_Meta_Lookup = std::function<uint8_t(int world_x, int world_y, int world_z)>;

Chunk_Meshes build_chunk_mesh(const Chunk_Block_Grid& blocks,
                              const Chunk_Light_Grid& light,
                              int chunk_x, int chunk_y, int chunk_z,
                              const Chunk_Neighbor_Lookup& neighbor_lookup,
                              const Chunk_Light_Lookup& light_lookup,
                              const Texture_Atlas& atlas,
                              bool only_positive_faces = false,
                              const Chunk_Meta_Lookup& meta_lookup = Chunk_Meta_Lookup{});

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
    void compact_light() { m_block_light.compact(); }
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
