//
// Created by noktemor on 11.02.2026.
//

#ifndef OPTICRAFT_CHUNK_H
#define OPTICRAFT_CHUNK_H

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#include "world/dimensions/Dimension.h"
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

// Граничные позиции секции: клетки на шести её гранях. Порядок: +X и -X — (z, y) по 32x8; +Y и -Y —
// (z, x) по 32x32; +Z и -Z — (x, y) по 32x8. Всего 3072. Переход между секциями идёт только через
// совпадающие позиции общей грани (одна и та же (u, v) у обеих секций), см. Section_Connectivity.
namespace Section_Boundary {
    inline constexpr int kCount = 2 * 256 + 2 * 1024 + 2 * 256;
    // Первая позиция каждой грани (порядок граней: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z).
    inline constexpr int kBase[6] = {0, 256, 512, 1536, 2560, 2816};
    // Слов в маске позиций грани (бит — локальная позиция p - kBase[грань]): 256 или 1024 бита.
    inline constexpr int kFaceWords[6] = {4, 4, 16, 16, 4, 4};
    inline constexpr uint8_t kNoComp = 0xFF;  // позиция непрозрачна
    inline constexpr int kMaxComps = 255;     // номера компонент 0..254, 0xFF занят под kNoComp
    inline int face_of(int p) {
        if (p < 256) return 0;
        if (p < 512) return 1;
        if (p < 1536) return 2;
        if (p < 2560) return 3;
        if (p < 2816) return 4;
        return 5;
    }
}

// Связность граничных клеток секции (см. get_renderable_chunks). Видимость через секцию — это переход
// от одной граничной клетки к совпадающей клетке соседней секции, поэтому грань целиком не годится:
// в пористых пещерах к одной грани прилегает много несвязанных полостей, и грубая модель склеивает их.
//   Open   — все граничные позиции проходимы и связаны между собой; значение по умолчанию (секция, для
//            которой связность ещё не посчитана, не отсекает то, что за ней), и для воздуха целиком.
//   Closed — проходимых граничных позиций нет: через секцию не пройти, видны только её собственные грани.
//   Mixed  — компоненты, касающиеся граничных позиций. Для каждой грани — записи Face_Comp (номер компоненты и
//            смещение её маски в masks): бит маски — локальная позиция грани. Позиции двух соседних граней
//            совпадают по локальному индексу (см. kBase), поэтому переход между секциями — пересечение масок.
//            Внутренние полости без выхода на границу сюда не попадают: через них обзор наружу не идёт.
struct Section_Connectivity {
    enum class Kind : uint8_t { Open, Closed, Mixed };
    struct Face_Comp {
        uint8_t comp;
        uint32_t mask;
        bool operator==(const Face_Comp&) const = default;
    };
    Kind kind = Kind::Open;
    uint16_t comp_count = 0;
    // Записи граней: face_comps[face_begin[f] .. face_begin[f + 1]), по возрастанию номера компоненты.
    std::array<uint16_t, 7> face_begin{};
    std::vector<Face_Comp> face_comps;
    std::vector<uint64_t> masks;
};

// Что нужно знать CPU о секции чанка (8 слоёв по Y). Сама геометрия живёт на видеокарте и
// строится компьют-шейдерами; здесь — только маска видов для отрисовки, границы для отсечения и связность.
struct Chunk_Section_Range {
    // Какие виды геометрии возможны в секции: 1 — непрозрачные кубы, 2 — блоки формы, 4 — флора, 8 — жидкость.
    uint8_t kinds = 0;
    float min_y = 0.0f;
    float max_y = 0.0f;
    bool has_geometry = false;
    // Связность для отсечения укрытий. Считается и для секций без геометрии: воздух тоже проводит обзор.
    Section_Connectivity conn;
};

// Счётчик изменений связности секций: растёт, когда у какого-либо чанка меняется Section_Connectivity. Смена
// мешей без изменения связности его не трогает. По нему кэш обхода достижимости (get_renderable_chunks)
// решает, нужен ли пересчёт.
uint64_t current_connectivity_epoch();

// Каналов полезной нагрузки GPU-мешера: 0 блоки, 1 метаданные, 2 свет своего чанка;
// 3..8 блоки граней соседей (-X,+X,-Y,+Y,-Z,+Z); 9..14 свет граней соседей; 15 — кольцо блоков
// рёбер и углов (AO у границ чанка, высоты жидкости); 16..21 метаданные граней соседей (уровень
// жидкости); 22 — метаданные кольца. См. mesh_greedy.comp и mesh_misc.comp.
// Канал 23 — coarse-сетка блоков для solid LOD; канал 24 — соответствующие метаданные,
// чтобы сохранять ориентацию и активное состояние блоков. Канал 0 остаётся полноразмерным
// для блоков сложной формы, флоры и жидкостей.
inline constexpr int kGpuMeshChannelCount = 25;
// Заголовок чанка в батче GPU: слот, (флаги, маска видов) + по одному uvec4 на канал.
inline constexpr int kGpuHeaderStride = 2 + kGpuMeshChannelCount;

// Кольцо для AO: клетки за границей чанка, у которых две или три координаты вне 0..size-1.
// Грани за одной границей берутся из шести снимков граней, а рёбра и углы (соседние по диагонали
// чанки) — отсюда. Порядок: 4 ребра вдоль Y (x и z вне), 4 вдоль X (y и z), 4 вдоль Z (x и y), 8 углов.
namespace Chunk_Ring {
    inline constexpr int kEdgeXBase = 4 * Config::chunk_height;
    inline constexpr int kEdgeZBase = kEdgeXBase + 4 * Config::chunk_size;
    inline constexpr int kCornerBase = kEdgeZBase + 4 * Config::chunk_size;
    inline constexpr int kSize = kCornerBase + 8;
}

// Индекс клетки кольца по координатам -1..size (ровно две или три координаты вне диапазона);
// -1 для всех остальных клеток. Должен совпадать с ring_index в mesh_greedy.comp.
inline int chunk_ring_index(int x, int y, int z) {
    constexpr int N = Config::chunk_size;
    constexpr int H = Config::chunk_height;
    const bool ox = x < 0 || x >= N;
    const bool oy = y < 0 || y >= H;
    const bool oz = z < 0 || z >= N;
    const int out = int(ox) + int(oy) + int(oz);
    if (out < 2) return -1;
    const int px = x >= N ? 1 : 0;  // 0 — сторона отрицательная, 1 — положительная
    const int py = y >= H ? 1 : 0;
    const int pz = z >= N ? 1 : 0;
    if (out == 3) return Chunk_Ring::kCornerBase + px + 2 * py + 4 * pz;
    if (!oy) return (px + 2 * pz) * H + y;
    if (!ox) return Chunk_Ring::kEdgeXBase + (py + 2 * pz) * N + x;
    return Chunk_Ring::kEdgeZBase + (px + 2 * py) * N + z;
}

// Обратное к chunk_ring_index: координаты клетки кольца относительно угла чанка.
inline void chunk_ring_coords(int index, int& x, int& y, int& z) {
    constexpr int N = Config::chunk_size;
    constexpr int H = Config::chunk_height;
    if (index < Chunk_Ring::kEdgeXBase) {
        const int e = index / H;
        x = (e & 1) ? N : -1;
        z = (e & 2) ? N : -1;
        y = index - e * H;
    } else if (index < Chunk_Ring::kEdgeZBase) {
        const int j = index - Chunk_Ring::kEdgeXBase;
        const int e = j / N;
        y = (e & 1) ? H : -1;
        z = (e & 2) ? N : -1;
        x = j - e * N;
    } else if (index < Chunk_Ring::kCornerBase) {
        const int j = index - Chunk_Ring::kEdgeZBase;
        const int e = j / N;
        x = (e & 1) ? N : -1;
        y = (e & 2) ? H : -1;
        z = j - e * N;
    } else {
        const int c = index - Chunk_Ring::kCornerBase;
        x = (c & 1) ? N : -1;
        y = (c & 2) ? H : -1;
        z = (c & 4) ? N : -1;
    }
}

// Всё, что CPU хранит о меше чанка. Вершин и квадов здесь нет: их строит видеокарта (см. Renderer).
struct Chunk_Meshes {
    std::array<Chunk_Section_Range, Config::chunk_section_count> sections;
    // Объединение видов геометрии всех секций (0 — рисовать нечего).
    uint8_t gpu_kinds = 0;
    // 1 = full detail, 2 = medium LOD, 4 = distant LOD.
    uint8_t lod_stride = 1;
    // Снимок области захвата (изометрия): отрицательные грани не строятся — рисуются только +X/+Y/+Z.
    bool positive_faces_only = false;
    // Полезная нагрузка для видеокарты (палитры и битово упакованные значения каналов).
    // mutable: Renderer забирает её при загрузке в GPU и очищает, поэтому после отправки
    // она не держится в RAM.
    mutable bool gpu_upload_pending = false;
    mutable std::vector<uint32_t> gpu_words;
    // На канал: смещение палитры, смещение упакованных слов, битность (0 = одно значение).
    // Смещения относительно начала этой нагрузки; Renderer переносит их при загрузке (submit_gpu_batch).
    mutable std::array<std::array<uint32_t, 3>, kGpuMeshChannelCount> gpu_channels{};
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
//   257..65536              16               64 КБ   (палитра переполнилась — ширина растёт сама)
//
// Ширины подобраны степенями двойки, чтобы чтение воксела сводилось к сдвигам и маскам без
// деления — get() вызывается в самых горячих циклах (мешинг, освещение).
// Палитра хранит полноценные Block_ID (uint16): ограничения на число видов блоков нет,
// кроме самого пространства id (k_max_block_types).
class Chunk_Block_Grid {
public:
    static constexpr size_t voxel_count = static_cast<size_t>(Config::chunk_size) *
                                           Config::chunk_height * Config::chunk_size;

private:
    std::vector<Block_ID> m_palette;  // номер в палитре -> id блока (всегда минимум 1 запись)
    std::vector<uint16_t> m_counts;   // сколько вокселей использует каждая запись палитры
    std::vector<uint64_t> m_words;    // упакованные номера; пусто, когда m_bits == 0
    uint8_t m_bits = 0;               // 0, 1, 2, 4, 8 или 16
    uint8_t m_log_bits = 0;           // log2(m_bits)
    uint8_t m_word_shift = 0;         // log2(вокселей в 64-битном слове)
    uint8_t m_slot_mask = 0;          // (вокселей в слове) - 1
    uint32_t m_value_mask = 0;        // (1 << m_bits) - 1

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
             : palette_size <= 16 ? 4 : palette_size <= 256 ? 8 : 16;
    }
    static constexpr size_t capacity_for_bits(uint8_t bits) {
        return bits == 0 ? 1 : (static_cast<size_t>(1) << bits);
    }

    void apply_bits(uint8_t bits);
    // Перепаковывает номера под новую ширину (remap — необязательная таблица старый -> новый номер).
    void repack(uint8_t new_bits, const uint16_t* remap = nullptr);

    uint16_t index_at(size_t i) const {
        if (m_bits == 0) return 0;
        const uint64_t word = m_words[i >> m_word_shift];
        const unsigned offset = (static_cast<unsigned>(i) & m_slot_mask) << m_log_bits;
        return static_cast<uint16_t>((word >> offset) & m_value_mask);
    }
    void write_index(size_t i, uint16_t palette_index) {
        uint64_t& word = m_words[i >> m_word_shift];
        const unsigned offset = (static_cast<unsigned>(i) & m_slot_mask) << m_log_bits;
        word = (word & ~(static_cast<uint64_t>(m_value_mask) << offset)) |
               (static_cast<uint64_t>(palette_index) << offset);
    }
    // Номер записи палитры для id блока; добавляет запись (при необходимости расширяя битность).
    uint16_t palette_index_for(Block_ID block_id);

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
        const Block_ID id = static_cast<Block_ID>(type);
        const uint16_t old_index = index_at(i);
        if (m_palette[old_index] == id) return;
        const uint16_t new_index = palette_index_for(id);   // может перепаковать m_words
        // old_index не меняется при перепаковке "на месте" (записи не переупорядочиваются)
        --m_counts[old_index];
        ++m_counts[new_index];
        write_index(i, new_index);
    }

    void fill(Block_Types type) {
        m_palette.assign(1, static_cast<Block_ID>(type));
        m_counts.assign(1, static_cast<uint16_t>(voxel_count));
        // Оставляем буфер упакованных слов у чанка: при повторном использовании
        // слота/чанка это исключает лишнюю пару free/alloc.
        m_words.clear();
        apply_bits(0);
        m_meta.clear();
    }

    // Загрузка/сохранение целиком "плоским" массивом id (voxel_count байт, порядок y,z,x).
    // Строит палитру за один проход — намного быстрее, чем voxel_count вызовов set().
    void assign_flat(const Block_ID* flat);
    void unpack(Block_ID* out) const;
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

    // Блоки, записанные в палитре (могут включать уже не используемые записи).
    const std::vector<Block_ID>& palette() const { return m_palette; }
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
        return m_words.size() * sizeof(uint64_t) + m_palette.size() * sizeof(Block_ID) +
               m_counts.size() * sizeof(uint16_t) +
               m_meta.size() * (sizeof(uint16_t) + sizeof(uint8_t));
    }
    constexpr size_t size_bytes() const { return voxel_count * sizeof(Block_ID); }  // размер распакованного вида
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
    std::array<Block_ID, static_cast<size_t>(Config::chunk_size) * Config::chunk_height> data{};
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

// Готовит данные чанка для GPU-мешеров: связность секций, маски видов геометрии и упакованную
// нагрузку (блоки, метаданные, свет, грани соседей и кольцо). Грани соседей, которых нет в снимке,
// достраиваются лукапами, и только там, где на собственной границе есть блок, которому нужны соседи.
Chunk_Meshes build_chunk_gpu_mesh(const Chunk_Block_Grid& blocks,
                                  const Chunk_Light_Grid& light,
                                  const Chunk_Block_Faces& neighbor_blocks,
                                  const Chunk_Light_Faces& neighbor_light,
                                  int chunk_x, int chunk_y, int chunk_z,
                                  const Chunk_Neighbor_Lookup& neighbor_lookup,
                                  const Chunk_Light_Lookup& light_lookup,
                                  const Chunk_Meta_Lookup& meta_lookup,
                                  bool only_positive_faces,
                                  int lod_stride = 1);

// Таблицы правил мешеров для GPU.
struct Gpu_Mesh_Tables {
    // Бит 0: жадно-мешаемый блок; 1: прозрачный; 2: uses_meta; 3: cull_same; 4: непрозрачный куб
    // (закрывает AO соседних углов); 5: флора; 6: жидкость; 7: блок формы; 8: закрывает грань блока
    // формы; 9: состояние «жидкость»; биты 10..12: форма (Block_Shape).
    std::vector<uint32_t> flags;   // по id до Block_Registry::id_capacity()
    // Упакованный свет (sky<<4 | block) -> огрублённый (сейчас тождественный).
    std::array<uint32_t, 256> light_coarse{};
    // Боксы блоков формы в 1/64 клетки: shape_index[(форма * 256 + метаданные) * 2] — число боксов,
    // следующий элемент — смещение (в парах) в shape_boxes; бокс = пара слов (см. mesh_misc.comp).
    std::vector<uint32_t> shape_index;
    std::vector<uint32_t> shape_boxes;
};
Gpu_Mesh_Tables gpu_mesh_tables();

// Полный непрозрачный куб (Mesh_Tables::occludes): клетка с таким блоком закрывает обзор. Тем же правилом строится
// связность секций (Section_Connectivity) и проверяется, не стоит ли глаз камеры внутри такого блока.
bool block_occludes_view(Block_Types type);

class Chunk {
private:
    int m_chunk_x;
    int m_chunk_y;
    int m_chunk_z;

    Chunk_Block_Grid m_blocks;
    Chunk_Light_Grid m_block_light; // packed: low nibble block light, high nibble sky light
    std::array<int, Config::chunk_height> m_layer_block_count{};

    const World_Generator* m_world_generator = nullptr;
    Dimension_Id m_dimension = Dimension_Id::Overworld;
    const Chunk_Manager* m_chunk_manager = nullptr;
    const Texture_Atlas* m_texture_atlas = nullptr;

    Chunk_Meshes m_meshes;
    bool m_is_mesh_dirty = true;
    static inline std::atomic<uint64_t> s_dirty_events{0};
    // Растёт при каждой пометке грязности (см. mark_mesh_dirty): задача пересборки запоминает его
    // на моменте снимка, и если к её применению он вырос, правка легла уже после снимка.
    uint64_t m_dirty_epoch = 0;
    uint64_t m_mesh_seq_issued = 0;
    // Уникальный номер ОБЪЕКТА (выдаётся в конструкторе из глобального счётчика, с 1). Позиция
    // для этого не годится: выгрузили и загрузили заново — позиция та же, объект уже другой.
    uint64_t m_instance_id = 0;
    // Отличает чанк, который игрок действительно правил, от нетронутого.
    // Нетронутый чанк полностью восстанавливается генератором по сиду, поэтому
    // писать его на диск бессмысленно: раньше выгрузка сохраняла ВСЕ чанки
    // подряд, то есть тысячи записей по 32 КБ при обычной ходьбе по миру.
    bool m_is_modified = false;
    uint64_t m_mesh_version = 0;
    // Кольцо относительно игрока (max по |dx|,|dz|,|dy|, 255 = неизвестно) и поколение вида, в котором оно
    // посчитано. Менеджер пересчитывает кольца при каждой смене чанка игрока.
    uint8_t m_view_ring = 255;
    uint32_t m_view_generation = 0;

public:
    Chunk(int chunk_x, int chunk_y, int chunk_z, const World_Generator& generator);
    // Измерение, которому принадлежит чанк: задаётся генератором при создании и определяет
    // ключ хранения (region-файлы разных измерений не смешиваются).
    Dimension_Id get_dimension_id() const { return m_dimension; }

    void set_chunk_manager(const Chunk_Manager* manager) { m_chunk_manager = manager; }
    void set_texture_atlas(const Texture_Atlas* atlas) { m_texture_atlas = atlas; }

    void generate_blocks();
    // Завершает пакетную генерацию: размещает декорации, уплотняет палитру и обновляет metadata.
    // already_known_air=true разрешён только после точной для генератора проверки chunk_is_definitely_air.
    void finish_generation(bool already_known_air = false,
                           bool layer_metadata_precomputed = false);
    // Used by World_Generator when per-layer occupied counts were computed during filling.
    void set_layer_block_counts(const std::array<int, Config::chunk_height>& counts) {
        m_layer_block_count = counts;
    }
    void recalculate_metadata();
    void apply_mesh(Chunk_Meshes meshes);
    void touch_mesh_version();

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
    Chunk_Meshes& get_meshes_mutable() { return m_meshes; }
    uint64_t get_mesh_version() const { return m_mesh_version; }
    uint8_t get_view_ring() const { return m_view_ring; }
    uint32_t get_view_generation() const { return m_view_generation; }
    void set_view_ring(uint8_t ring, uint32_t generation) {
        m_view_ring = ring;
        m_view_generation = generation;
    }
    bool is_mesh_dirty() const { return m_is_mesh_dirty; }
    bool is_modified() const { return m_is_modified; }
    void mark_modified() { m_is_modified = true; }
    void clear_modified() { m_is_modified = false; }
    void mark_mesh_dirty() {
        m_is_mesh_dirty = true;
        ++m_dirty_epoch;
        s_dirty_events.fetch_add(1, std::memory_order_relaxed);
    }
    // Сколько раз ЛЮБОЙ чанк помечали грязным: менеджер по нему узнаёт, что скан грязных чанков
    // можно пропустить (с прошлого скана никто не стал грязным).
    static uint64_t dirty_event_count() { return s_dirty_events.load(std::memory_order_relaxed); }
    uint64_t get_dirty_epoch() const { return m_dirty_epoch; }
    // Номер последней поставленной задачи пересборки меша этого чанка. Результат более старой задачи
    // (например, поставленной до срочной пересборки после правки блока) отбрасывается.
    uint64_t next_mesh_seq() { return ++m_mesh_seq_issued; }
    uint64_t get_mesh_seq_issued() const { return m_mesh_seq_issued; }
    uint64_t get_instance_id() const { return m_instance_id; }
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
