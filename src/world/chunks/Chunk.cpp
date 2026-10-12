//
// Created by noktemor on 11.02.2026.
//

#include "world/chunks/Chunk.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Types.h"
#include "math/Math_Helpers.h"
#include "world/generation/World_Generator.h"
#include "rendering/atlas/Texture_Atlas.h"
#include "utils/logging/Logger.h"
#include <atomic>
#include <cmath>
#include <limits>
#include <algorithm>
#include <cstring>
#include <memory>
#include <array>

namespace {
    std::atomic<uint64_t> g_mesh_version_counter{0};
}

// ---------------------------------------------------------------------------
//  Chunk_Block_Grid: палитра + упакованные номера (описание формата — в Chunk.h)
// ---------------------------------------------------------------------------
void Chunk_Block_Grid::apply_bits(uint8_t bits) {
    m_bits = bits;
    m_log_bits = bits == 8 ? 3 : bits == 4 ? 2 : bits == 2 ? 1 : 0;
    m_word_shift = static_cast<uint8_t>(6 - m_log_bits);
    m_slot_mask = static_cast<uint8_t>((1u << m_word_shift) - 1u);
    m_value_mask = bits == 0 ? 0 : static_cast<uint8_t>((1u << bits) - 1u);
}

void Chunk_Block_Grid::repack(uint8_t new_bits, const uint8_t* remap) {
    std::vector<uint8_t> indices(voxel_count);
    for (size_t i = 0; i < voxel_count; ++i) {
        const uint8_t old_index = index_at(i);
        indices[i] = remap ? remap[old_index] : old_index;
    }

    apply_bits(new_bits);
    if (new_bits == 0) {
        m_words.clear();
        m_words.shrink_to_fit();
        return;
    }

    const size_t per_word = static_cast<size_t>(1) << m_word_shift;
    m_words.assign(voxel_count / per_word, 0);
    for (size_t w = 0; w < m_words.size(); ++w) {
        uint64_t packed = 0;
        for (size_t k = 0; k < per_word; ++k) {
            packed |= static_cast<uint64_t>(indices[w * per_word + k]) << (k * new_bits);
        }
        m_words[w] = packed;
    }
}

uint8_t Chunk_Block_Grid::palette_index_for(uint8_t block_id) {
    for (size_t i = 0; i < m_palette.size(); ++i) {
        if (m_palette[i] == block_id) return static_cast<uint8_t>(i);
    }
    // Запись, которую больше никто не использует (блок целиком заменили), можно занять
    // повторно — так палитра не растёт при активной правке чанка.
    for (size_t i = 0; i < m_palette.size(); ++i) {
        if (m_counts[i] == 0) {
            m_palette[i] = block_id;
            return static_cast<uint8_t>(i);
        }
    }
    if (m_palette.size() >= capacity_for_bits(m_bits)) {
        repack(bits_for_palette(m_palette.size() + 1));
    }
    m_palette.push_back(block_id);
    m_counts.push_back(0);
    return static_cast<uint8_t>(m_palette.size() - 1);
}

void Chunk_Block_Grid::assign_flat(const uint8_t* flat) {
    uint16_t slot_of[256];
    std::fill(std::begin(slot_of), std::end(slot_of), static_cast<uint16_t>(0xFFFFu));

    m_palette.clear();
    m_counts.clear();
    for (size_t i = 0; i < voxel_count; ++i) {
        const uint8_t id = flat[i];
        if (slot_of[id] == 0xFFFFu) {
            slot_of[id] = static_cast<uint16_t>(m_palette.size());
            m_palette.push_back(id);
            m_counts.push_back(0);
        }
        ++m_counts[slot_of[id]];
    }

    apply_bits(bits_for_palette(m_palette.size()));
    m_meta.clear();
    if (m_bits == 0) {
        m_words.clear();
        m_words.shrink_to_fit();
        return;
    }

    const size_t per_word = static_cast<size_t>(1) << m_word_shift;
    m_words.assign(voxel_count / per_word, 0);
    for (size_t w = 0; w < m_words.size(); ++w) {
        uint64_t packed = 0;
        const uint8_t* src = flat + w * per_word;
        for (size_t k = 0; k < per_word; ++k) {
            packed |= static_cast<uint64_t>(slot_of[src[k]]) << (k * m_bits);
        }
        m_words[w] = packed;
    }
}

void Chunk_Block_Grid::unpack(uint8_t* out) const {
    if (m_bits == 0) {
        std::memset(out, m_palette[0], voxel_count);
        return;
    }
    const size_t per_word = static_cast<size_t>(1) << m_word_shift;
    size_t i = 0;
    for (const uint64_t word : m_words) {
        for (size_t k = 0; k < per_word; ++k) {
            out[i++] = m_palette[(word >> (k * m_bits)) & m_value_mask];
        }
    }
}

void Chunk_Block_Grid::compact() {
    size_t live = 0;
    for (const uint16_t c : m_counts) {
        if (c > 0) ++live;
    }
    if (live == m_palette.size() && bits_for_palette(live) == m_bits) return;

    std::vector<uint8_t> remap(m_palette.size(), 0);
    std::vector<uint8_t> new_palette;
    std::vector<uint16_t> new_counts;
    for (size_t i = 0; i < m_palette.size(); ++i) {
        if (m_counts[i] == 0) continue;
        remap[i] = static_cast<uint8_t>(new_palette.size());
        new_palette.push_back(m_palette[i]);
        new_counts.push_back(m_counts[i]);
    }
    repack(bits_for_palette(new_palette.size()), remap.data());
    m_palette = std::move(new_palette);
    m_counts = std::move(new_counts);
}


Chunk::Chunk(int chunk_x, int chunk_y, int chunk_z, const World_Generator& generator)
    : m_chunk_x(chunk_x)
    , m_chunk_y(chunk_y)
    , m_chunk_z(chunk_z)
    , m_world_generator(&generator)
{
    // Сетка блоков создаётся целиком из воздуха (палитра из одной записи) — заполнять нечем.
}

void Chunk::recalculate_metadata() {
    m_layer_block_count.fill(0);
    if (m_blocks.is_uniform()) {
        if (m_blocks.uniform_block() != Block_Types::Air) {
            m_layer_block_count.fill(Config::chunk_size * Config::chunk_size);
        }
        return;
    }
    std::vector<uint8_t> flat(Chunk_Block_Grid::voxel_count);
    m_blocks.unpack(flat.data());
    constexpr size_t layer = static_cast<size_t>(Config::chunk_size) * Config::chunk_size;
    const uint8_t air = static_cast<uint8_t>(static_cast<Block_ID>(Block_Types::Air));
    for (int y = 0; y < Config::chunk_height; ++y) {
        const uint8_t* row = flat.data() + static_cast<size_t>(y) * layer;
        int count = 0;
        for (size_t i = 0; i < layer; ++i) count += (row[i] != air);
        m_layer_block_count[y] = count;
    }
}

void Chunk::generate_blocks() {
    if (!m_world_generator) {
        LOG_ERROR("Chunk::generate_blocks: World_Generator is nullptr!");
        return;
    }

    const int start_x = m_chunk_x * Config::chunk_size;
    const int start_z = m_chunk_z * Config::chunk_size;

    // Чанк целиком выше рельефа, воды и парящих островов — генерировать нечего.
    // На вертикальный столбец из 17 чанков таких обычно 10-14 штук.
    if (m_world_generator->chunk_is_definitely_air(m_chunk_x, m_chunk_y, m_chunk_z)) {
        m_blocks.fill(Block_Types::Air);
        m_layer_block_count.fill(0);
        m_is_modified = false;
        return;
    }

    // Пакетная генерация: климат и высота считаются один раз на колонку, а не на
    // каждый из 32768 вокселей (см. World_Generator::generate_chunk).
    m_world_generator->generate_chunk(m_blocks, m_chunk_x, m_chunk_y, m_chunk_z);

    // Generate decorations from nearby source columns too, so a tree or
    // structure crossing a border is reproduced by both affected chunks.
    for (int x = -Config::decoration_overhang;
         x < Config::chunk_size + Config::decoration_overhang; ++x) {
        for (int z = -Config::decoration_overhang;
             z < Config::chunk_size + Config::decoration_overhang; ++z) {
            m_world_generator->decorate_column(
                *this,
                start_x + x,
                start_z + z
            );
        }
    }

    // Декорации правили сетку через set() — выкидываем ставшие ненужными записи палитры.
    m_blocks.compact();
    recalculate_metadata();

    // Декорации ставятся через set_block и взвели бы флаг правки, хотя чанк
    // ровно такой, каким его выдаёт генератор.
    m_is_modified = false;
}

namespace {
    void push_face_impl(std::vector<Chunk_Vertex>& target,
                   const glm::vec4& uv,
                   float wx, float wy, float wz,
                   int dx, int dy, int dz,
                   float face_light) {
        const float u_min = uv.x;
        const float v_min = uv.y;
        const float u_max = uv.z;
        const float v_max = uv.w;

        const float nx = static_cast<float>(dx);
        const float ny = static_cast<float>(dy);
        const float nz = static_cast<float>(dz);
        const float l = face_light;

        if (dx != 0) {
            // Боковая грань (±X): картинка текстуры мира заведомо не квадратно-симметрична по
            // вертикали (трава — самый заметный пример: зелёный верх, земля снизу), поэтому V
            // обязан совпадать по направлению с реальным верхом/низом блока. get_uv_coords()
            // возвращает v_max = верх картинки, v_min = низ картинки (см. её комментарий и
            // flip-on-load в Texture::load_from_file — вместе они и задают это направление).
            // Значит низ блока (wy) должен получить v_min, а верх блока (wy+1) — v_max; здесь
            // было ровно наоборот, отчего боковые текстуры рисовались перевёрнутыми.
            const float fx = wx + (dx > 0 ? 1.0f : 0.0f);
            target.push_back({fx, wy,       wz,       u_min, v_min, nx, ny, nz, l});
            target.push_back({fx, wy + 1.0f, wz,       u_min, v_max, nx, ny, nz, l});
            target.push_back({fx, wy + 1.0f, wz + 1.0f, u_max, v_max, nx, ny, nz, l});
            target.push_back({fx, wy,       wz,       u_min, v_min, nx, ny, nz, l});
            target.push_back({fx, wy + 1.0f, wz + 1.0f, u_max, v_max, nx, ny, nz, l});
            target.push_back({fx, wy,       wz + 1.0f, u_max, v_min, nx, ny, nz, l});
            if (dx < 0) {
                std::swap(target[target.size() - 5], target[target.size() - 4]);
                std::swap(target[target.size() - 2], target[target.size() - 1]);
            }
        } else if (dy != 0) {
            const float fy = wy + (dy > 0 ? 1.0f : 0.0f);
            target.push_back({wx,       fy, wz,       u_min, v_max, nx, ny, nz, l});
            target.push_back({wx + 1.0f, fy, wz,       u_max, v_max, nx, ny, nz, l});
            target.push_back({wx + 1.0f, fy, wz + 1.0f, u_max, v_min, nx, ny, nz, l});
            target.push_back({wx,       fy, wz,       u_min, v_max, nx, ny, nz, l});
            target.push_back({wx + 1.0f, fy, wz + 1.0f, u_max, v_min, nx, ny, nz, l});
            target.push_back({wx,       fy, wz + 1.0f, u_min, v_min, nx, ny, nz, l});
            if (dy > 0) {
                // ИСПРАВЛЕНО: было `dy < 0` — базовый (не свапнутый) обход вершин уже даёт
                // нормаль (0,-1,0), то есть верно для нижней грани (dy<0) без свапа, а свап
                // нужен именно для ВЕРХНЕЙ (dy>0), чтобы развернуть его на (0,1,0). Со старым
                // условием обе Y-грани были закручены в обратную сторону: с GL_CULL_FACE
                // включённым (Window.cpp) верх/низ блока отбрасывались backface culling'ом
                // именно с той стороны, откуда их положено видеть — подтверждено изолированным
                // тестом (рендер одной грани, чтение центрального пикселя framebuffer'а).
                std::swap(target[target.size() - 5], target[target.size() - 4]);
                std::swap(target[target.size() - 2], target[target.size() - 1]);
            }
        } else {
            // Боковая грань (±Z) — то же рассуждение и тот же разворот v_min/v_max, что и в
            // ветке dx!=0 выше.
            const float fz = wz + (dz > 0 ? 1.0f : 0.0f);
            target.push_back({wx,       wy,       fz, u_min, v_min, nx, ny, nz, l});
            target.push_back({wx + 1.0f, wy,       fz, u_max, v_min, nx, ny, nz, l});
            target.push_back({wx + 1.0f, wy + 1.0f, fz, u_max, v_max, nx, ny, nz, l});
            target.push_back({wx,       wy,       fz, u_min, v_min, nx, ny, nz, l});
            target.push_back({wx + 1.0f, wy + 1.0f, fz, u_max, v_max, nx, ny, nz, l});
            target.push_back({wx,       wy + 1.0f, fz, u_min, v_max, nx, ny, nz, l});
            if (dz < 0) {
                // ИСПРАВЛЕНО: было `dz > 0` — та же ошибка направления, что и у dy выше
                // (базовый обход уже даёт нормаль (0,0,1), верную для dz>0 без свапа; свап
                // нужен для dz<0, а не для dz>0).
                std::swap(target[target.size() - 5], target[target.size() - 4]);
                std::swap(target[target.size() - 2], target[target.size() - 1]);
            }
        }
    }

    // Поворачивает UV последних 6 вершин (один квад) на 90° внутри прямоугольника спрайта.
    // Спрайты квадратные, так что достаточно обменять роли осей: (su, sv) -> (sv, 1 - su).
    void rotate_last_quad_uv(std::vector<Chunk_Vertex>& target, const glm::vec4& uv) {
        for (size_t i = target.size() - 6; i < target.size(); ++i) {
            Chunk_Vertex& v = target[i];
            const bool su = (v.u == uv.z);
            const bool sv = (v.v == uv.w);
            const bool new_su = sv;
            const bool new_sv = !su;
            v.u = new_su ? uv.z : uv.x;
            v.v = new_sv ? uv.w : uv.y;
        }
    }

    void push_face(std::vector<Chunk_Vertex>& target,
                   const glm::vec4& uv,
                   float wx, float wy, float wz,
                   int dx, int dy, int dz,
                   float face_light,
                   bool rotate_quarter = false) {
        push_face_impl(target, uv, wx, wy, wz, dx, dy, dz, face_light);
        if (rotate_quarter) rotate_last_quad_uv(target, uv);
    }

    int face_index(int dx, int dy, int dz) {
        if (dx > 0) return Block_Face::pos_x;
        if (dx < 0) return Block_Face::neg_x;
        if (dy > 0) return Block_Face::pos_y;
        if (dy < 0) return Block_Face::neg_y;
        return dz > 0 ? Block_Face::pos_z : Block_Face::neg_z;
    }

    // Грань жидкости. Жидкость не сливается жадным мешером: у каждой клетки своя высота
    // поверхности (уровень течения). ylo..yhi — вертикальный диапазон боковой грани внутри клетки
    // (0..h — вся стенка, hn..h — "ступенька" над более низким соседом); для верхней грани yhi —
    // высота поверхности, нижняя лежит на дне клетки.
    void add_liquid_face_data(Chunk_Meshes& meshes,
                              int chunk_x, int chunk_y, int chunk_z,
                              int x, int y, int z, int dx, int dy, int dz, Block_Types block,
                              float face_light, float ylo, float yhi) {
        const float wx = static_cast<float>(chunk_x * Config::chunk_size + x);
        const float wy = static_cast<float>(chunk_y * Config::chunk_height + y);
        const float wz = static_cast<float>(chunk_z * Config::chunk_size + z);
        const Block_Properties& props = get_block_props(block);
        const glm::vec4& uv = props.get_uv(dy);

        auto& target = meshes.liquid;
        const size_t first = target.size();
        push_face_impl(target, uv, wx, wy, wz, dx, dy, dz, face_light);

        if (dy > 0) {
            for (size_t i = first; i < target.size(); ++i) target[i].y = wy + yhi;
        } else if (dy == 0) {
            const float v0 = uv.y, v1 = uv.w;
            for (size_t i = first; i < target.size(); ++i) {
                const float t = target[i].y - wy;           // 0 — низ клетки, 1 — верх
                const float frac = ylo + t * (yhi - ylo);
                target[i].y = wy + frac;
                target[i].v = v0 + (v1 - v0) * frac;
            }
        }
    }

    void add_flora_block_data(Chunk_Meshes& meshes,
                              int chunk_x, int chunk_y, int chunk_z,
                              int x, int y, int z, Block_Types block, float own_light) {
        const float wx = static_cast<float>(chunk_x * Config::chunk_size + x);
        const float wy = static_cast<float>(chunk_y * Config::chunk_height + y);
        const float wz = static_cast<float>(chunk_z * Config::chunk_size + z);

        const Block_Properties& props = get_block_props(block);
        const glm::vec4& uv = props.uv_top;
        const float u_min = uv.x, v_min = uv.y, u_max = uv.z, v_max = uv.w;
        const float l = own_light;

        // Два перекрёстных квада (X-style), как трава/цветы в оригинале.
        auto& target = meshes.flora;
        const float x0 = wx, x1 = wx + 1.0f;
        const float z0 = wz, z1 = wz + 1.0f;
        const float y0 = wy, y1 = wy + 1.0f;

        // Верхние вершины (y1) помечены nx=1 — это сигнал для flora_vertex.glsl "качать этот
        // угол на ветру", а не настоящая нормаль (у флоры она всё равно не боковая). Через
        // UV такой флаг надёжно не сделать: v_max этого тайла численно совпадает с v_min
        // соседнего (тайлы стыкуются), так что "верх"/"низ" по одному v не отличить —
        // поймано ровно на этом изолированным тестом. nx не участвует в текущей модели
        // освещения (там смотрят только на Normal.y), так что на подсветку это не влияет.
        //
        // У каждого квада ОДИН порядок обхода вершин, то есть одна лицевая сторона.
        // Чтобы цветок был виден со всех четырёх направлений, а не с двух, проход
        // флоры рисуется с выключенным GL_CULL_FACE (см. Renderer::render_chunks).
        // Дублировать здесь геометрию с обратным обходом не нужно: это удвоило бы
        // и память под меш, и число обрабатываемых вершин ради того же результата.
        // V: get_uv_coords() отдаёт v_max = ВЕРХ картинки, v_min = НИЗ (см. Texture_Atlas и flip-on-load),
        // поэтому низ квада (y0) получает v_min, а верх (y1) — v_max. Раньше было наоборот,
        // и цветы/трава рисовались вверх ногами.
        target.push_back({x0, y0, z0, u_min, v_min, 0, 1, 0, l});
        target.push_back({x1, y0, z1, u_max, v_min, 0, 1, 0, l});
        target.push_back({x1, y1, z1, u_max, v_max, 1, 1, 0, l});
        target.push_back({x0, y0, z0, u_min, v_min, 0, 1, 0, l});
        target.push_back({x1, y1, z1, u_max, v_max, 1, 1, 0, l});
        target.push_back({x0, y1, z0, u_min, v_max, 1, 1, 0, l});

        target.push_back({x1, y0, z0, u_min, v_min, 0, 1, 0, l});
        target.push_back({x0, y0, z1, u_max, v_min, 0, 1, 0, l});
        target.push_back({x0, y1, z1, u_max, v_max, 1, 1, 0, l});
        target.push_back({x1, y0, z0, u_min, v_min, 0, 1, 0, l});
        target.push_back({x0, y1, z1, u_max, v_max, 1, 1, 0, l});
        target.push_back({x1, y1, z0, u_min, v_max, 1, 1, 0, l});
    }
}

// ---------------------------------------------------------------------------
//  Жадный меш (greedy meshing)
// ---------------------------------------------------------------------------
// Раньше каждая видимая грань каждого блока была отдельным квадом (6 вершин). Стена 10x10 из
// одинакового камня давала 100 квадов. Здесь одинаковые соседние грани сливаются в прямоугольники:
// стена — 1 квад. Сливаются грани, у которых совпадает ВСЁ, что влияет на внешний вид: блок,
// метаданные (ориентация/состояние) и освещение (клетка, в которую смотрит грань) — поэтому
// картинка остаётся пиксель-в-пиксель прежней, а число вершин падает в разы.
//
// Квад не пересекает границу Y-секции (Config::chunk_section_height): рендерер рисует и отсекает
// чанк по секциям, и у каждой секции свой диапазон вершин (Chunk_Section_Range).
// Жидкость и флора не сливаются: у жидкости своя высота поверхности, флора — крест-накрест.
namespace {
    constexpr int kN = Config::chunk_size;
    constexpr int kH = Config::chunk_height;
    static_assert(kN == kH, "greedy mesher assumes cubic chunks");
    constexpr size_t kLayer = static_cast<size_t>(kN) * kN;
    constexpr size_t kVoxels = kLayer * kH;

    // Биты ключа слияния грани (см. комментарий выше): id | meta<<8 | light<<16 | флаги.
    constexpr uint32_t kKeyPresent = 1u << 24;
    constexpr uint32_t kKeyRotate = 1u << 25;   // повёрнутая текстура — такой квад не сливается

    // Свойства блока по id: реестр неизменен после загрузки blocks.json, а Block_Properties —
    // тяжёлая структура, поэтому в горячем цикле держим только указатели.
    const std::array<const Block_Properties*, k_max_block_types>& props_table() {
        static const std::array<const Block_Properties*, k_max_block_types> table = [] {
            std::array<const Block_Properties*, k_max_block_types> t{};
            for (size_t i = 0; i < t.size(); ++i) {
                t[i] = &get_block_props(static_cast<Block_ID>(i));
            }
            return t;
        }();
        return table;
    }

    // Плоские таблицы "id -> нужный бит" (256 байт каждая, целиком в L1): в цикле построения маски
    // каждая клетка проверяется по ним, а не через Block_Properties (тяжёлая структура).
    struct Mesh_Tables {
        std::array<uint8_t, k_max_block_types> greedy{};      // твёрдый блок, сливается жадным мешером
        std::array<uint8_t, k_max_block_types> transparent{}; // грань к такому соседу видна
        std::array<uint8_t, k_max_block_types> uses_meta{};
        // Полупрозрачный кубический блок (листва, стекло, лёд, слизь): грань к такому же блоку
        // не рисуется (внутри кроны/глыбы граней нет), а свет грани огрубляется (light_coarse),
        // чтобы соседние грани сливались в большие квады, а не дробились из-за 1 уровня света.
        std::array<uint8_t, k_max_block_types> cull_same{};
        // Упакованный свет (sky<<4 | block) -> огрублённый до 6 уровней на канал (0,3,6,9,12,15).
        std::array<uint8_t, 256> light_coarse{};
    };
    const Mesh_Tables& mesh_tables() {
        static const Mesh_Tables tables = [] {
            Mesh_Tables m;
            for (size_t i = 0; i < k_max_block_types; ++i) {
                const Block_Properties& p = *props_table()[i];
                m.greedy[i] = (i != 0 && p.state != Block_State::Liquid &&
                               p.mesh_style != Block_Mesh_Style::XStyle) ? 1 : 0;
                m.transparent[i] = p.is_transparent ? 1 : 0;
                m.uses_meta[i] = p.uses_meta ? 1 : 0;
                m.cull_same[i] = (m.greedy[i] && p.is_transparent) ? 1 : 0;
            }
            for (int packed = 0; packed < 256; ++packed) {
                const int sky = packed >> 4, blk = packed & 15;
                const int sky_q = ((sky * 5 + 7) / 15) * 3;
                const int blk_q = ((blk * 5 + 7) / 15) * 3;
                m.light_coarse[static_cast<size_t>(packed)] = static_cast<uint8_t>((sky_q << 4) | blk_q);
            }
            return m;
        }();
        return tables;
    }

    // Рабочие буферы мешера — свои у каждого потока (мешинг идёт в воркерах), чтобы не
    // аллоцировать ~100 КБ на каждую задачу.
    struct Mesh_Scratch {
        std::array<uint8_t, kVoxels> blocks;       // id блоков чанка, плоско (y, z, x)
        std::array<uint8_t, kVoxels> light;        // упакованный свет, плоско
        // Соседние клетки за границей чанка запрашиваются лениво и кэшируются: за пределами
        // загруженного мира блок спрашивается у генератора (дорого), поэтому берём только те
        // клетки, которые реально понадобились.
        std::array<uint8_t, 6 * kLayer> border_block, border_light;
        std::array<uint8_t, 6 * kLayer> block_known, light_known;
        std::array<uint32_t, kLayer> mask;
    };

    Mesh_Scratch& mesh_scratch() {
        thread_local std::unique_ptr<Mesh_Scratch> scratch = std::make_unique<Mesh_Scratch>();
        return *scratch;
    }

    // Индекс клетки за границей чанка в кэше соседей (грань: -X,+X,-Y,+Y,-Z,+Z).
    inline size_t border_slot(int x, int y, int z) {
        if (x < 0 || x >= kN) return (x >= kN ? 1 : 0) * kLayer + static_cast<size_t>(y) * kN + z;
        if (y < 0 || y >= kH) return (y >= kH ? 3 : 2) * kLayer + static_cast<size_t>(z) * kN + x;
        return (z >= kN ? 5 : 4) * kLayer + static_cast<size_t>(y) * kN + x;
    }

    // Один квад непрозрачной геометрии. w — размер вдоль оси u, h — вдоль оси v (см. kAxisU/kAxisV
    // ниже), (wx, wy, wz) — мировые координаты угла клетки с минимальными координатами.
    // Порядок вершин и ориентация UV — те же, что были у одиночной грани (push_face_impl), только
    // с растяжкой на w x h: чтобы текстура не растягивалась, локальные UV идут от 0 до w / h.
    void emit_terrain_quad(std::vector<Terrain_Vertex>& out,
                           float wx, float wy, float wz,
                           int axis, int sign, int w, int h,
                           const glm::vec4& uv, float light, bool rotate) {
        const float ny = axis == 1 ? static_cast<float>(sign) : 0.0f;
        const float U = static_cast<float>(w);
        const float V = static_cast<float>(h);
        const size_t first = out.size();

        const auto push = [&](float x, float y, float z, float a, float b) {
            out.push_back({x, y, z, a, b, ny, light, uv.x, uv.y, uv.z, uv.w});
        };

        if (axis == 0) {            // грань ±X: u вдоль Z, v вдоль Y
            const float fx = wx + (sign > 0 ? 1.0f : 0.0f);
            const float sy = V, sz = U;
            push(fx, wy,      wz,      0, 0);
            push(fx, wy + sy, wz,      0, V);
            push(fx, wy + sy, wz + sz, U, V);
            push(fx, wy,      wz,      0, 0);
            push(fx, wy + sy, wz + sz, U, V);
            push(fx, wy,      wz + sz, U, 0);
            if (sign < 0) {
                std::swap(out[first + 1], out[first + 2]);
                std::swap(out[first + 4], out[first + 5]);
            }
        } else if (axis == 1) {     // грань ±Y: u вдоль X, v вдоль Z
            const float fy = wy + (sign > 0 ? 1.0f : 0.0f);
            const float sx = U, sz = V;
            push(wx,      fy, wz,      0, V);
            push(wx + sx, fy, wz,      U, V);
            push(wx + sx, fy, wz + sz, U, 0);
            push(wx,      fy, wz,      0, V);
            push(wx + sx, fy, wz + sz, U, 0);
            push(wx,      fy, wz + sz, 0, 0);
            if (sign > 0) {
                std::swap(out[first + 1], out[first + 2]);
                std::swap(out[first + 4], out[first + 5]);
            }
        } else {                    // грань ±Z: u вдоль X, v вдоль Y
            const float fz = wz + (sign > 0 ? 1.0f : 0.0f);
            const float sx = U, sy = V;
            push(wx,      wy,      fz, 0, 0);
            push(wx + sx, wy,      fz, U, 0);
            push(wx + sx, wy + sy, fz, U, V);
            push(wx,      wy,      fz, 0, 0);
            push(wx + sx, wy + sy, fz, U, V);
            push(wx,      wy + sy, fz, 0, V);
            if (sign < 0) {
                std::swap(out[first + 1], out[first + 2]);
                std::swap(out[first + 4], out[first + 5]);
            }
        }

        if (rotate) {
            // Поворот текстуры на 90° (бревно на боку) — только для квадов 1x1:
            // (a, b) -> (b, 1 - a), как раньше делал rotate_last_quad_uv.
            for (size_t i = first; i < out.size(); ++i) {
                const float a = out[i].local_u;
                const float b = out[i].local_v;
                out[i].local_u = b;
                out[i].local_v = 1.0f - a;
            }
        }
    }

    // Для грани с нормалью по оси a: ось u и ось v в плоскости грани.
    //   X -> u = Z, v = Y     Y -> u = X, v = Z     Z -> u = X, v = Y
    constexpr int kAxisU[3] = {2, 0, 0};
    constexpr int kAxisV[3] = {1, 2, 1};
}

Chunk_Meshes build_chunk_mesh(const Chunk_Block_Grid& blocks,
                              const Chunk_Light_Grid& light,
                              int chunk_x, int chunk_y, int chunk_z,
                              const Chunk_Neighbor_Lookup& neighbor_lookup,
                              const Chunk_Light_Lookup& light_lookup,
                              const Texture_Atlas& atlas,
                              bool only_positive_faces,
                              const Chunk_Meta_Lookup& meta_lookup) {
    // UV блоков резолвятся один раз при загрузке blocks.json (см. Block_Registry::
    // load_from_file → resolve_sprite_uv) и лежат готовыми в Block_Properties, так что atlas
    // для самой геометрии не нужен; параметр оставлен как "атлас обязан быть загружен".
    (void)atlas;
    Chunk_Meshes meshes;

    const int base_x = chunk_x * kN;
    const int base_y = chunk_y * kH;
    const int base_z = chunk_z * kN;

    // Чанк целиком из воздуха ничего не рисует (грани на границе рисуют соседи).
    const bool all_air = blocks.is_uniform() && blocks.uniform_block() == Block_Types::Air;

    Mesh_Scratch& S = mesh_scratch();
    const auto& props = props_table();

    std::array<int, Config::chunk_height> layer_block_count{};
    if (!all_air) {
        blocks.unpack(S.blocks.data());
        light.unpack(S.light.data());
        std::fill(S.block_known.begin(), S.block_known.end(), static_cast<uint8_t>(0));
        std::fill(S.light_known.begin(), S.light_known.end(), static_cast<uint8_t>(0));

        // Layer culling: слои без единого блока пропускаются (дёшево — 32x32 на слой).
        for (int y = 0; y < kH; ++y) {
            const uint8_t* row = S.blocks.data() + static_cast<size_t>(y) * kLayer;
            int count = 0;
            for (size_t i = 0; i < kLayer; ++i) count += (row[i] != 0);
            layer_block_count[y] = count;
        }
    }

    // Блок / свет в клетке чанка или в соседней клетке за его границей (ровно одна координата
    // выходит за диапазон: диагональных соседей мешер не спрашивает).
    const auto block_at = [&](int x, int y, int z) -> uint8_t {
        if (static_cast<unsigned>(x) < static_cast<unsigned>(kN) &&
            static_cast<unsigned>(y) < static_cast<unsigned>(kH) &&
            static_cast<unsigned>(z) < static_cast<unsigned>(kN)) {
            return S.blocks[(static_cast<size_t>(y) * kN + z) * kN + x];
        }
        const size_t slot = border_slot(x, y, z);
        if (!S.block_known[slot]) {
            S.block_known[slot] = 1;
            S.border_block[slot] = neighbor_lookup
                ? static_cast<uint8_t>(static_cast<Block_ID>(
                      neighbor_lookup(base_x + x, base_y + y, base_z + z)))
                : static_cast<uint8_t>(0);
        }
        return S.border_block[slot];
    };
    const auto light_at = [&](int x, int y, int z) -> uint8_t {
        if (static_cast<unsigned>(x) < static_cast<unsigned>(kN) &&
            static_cast<unsigned>(y) < static_cast<unsigned>(kH) &&
            static_cast<unsigned>(z) < static_cast<unsigned>(kN)) {
            return S.light[(static_cast<size_t>(y) * kN + z) * kN + x];
        }
        const size_t slot = border_slot(x, y, z);
        if (!S.light_known[slot]) {
            S.light_known[slot] = 1;
            S.border_light[slot] = light_lookup
                ? light_lookup(base_x + x, base_y + y, base_z + z)
                : static_cast<uint8_t>(0);
        }
        return S.border_light[slot];
    };

    // Метаданные клетки в чанке или за его границей (для уровня жидкости).
    const auto meta_at = [&](int x, int y, int z) -> uint8_t {
        if (static_cast<unsigned>(x) < static_cast<unsigned>(kN) &&
            static_cast<unsigned>(y) < static_cast<unsigned>(kH) &&
            static_cast<unsigned>(z) < static_cast<unsigned>(kN)) {
            return blocks.get_meta(x, y, z);
        }
        return meta_lookup ? meta_lookup(base_x + x, base_y + y, base_z + z) : static_cast<uint8_t>(0);
    };

    // Высота поверхности жидкости в клетке (0, если там не жидкость). Как в Minecraft: уровень
    // amount/9, а если над клеткой та же жидкость — полный блок (иначе в столбе воды были бы щели).
    const auto liquid_height = [&](int x, int y, int z, uint8_t id) -> float {
        const bool x_in = static_cast<unsigned>(x) < static_cast<unsigned>(kN);
        const bool z_in = static_cast<unsigned>(z) < static_cast<unsigned>(kN);
        // Над клеткой на самом краю чанка (две оси сразу за границей) мешер соседей не знает.
        const bool can_look_up = (y + 1 < kH) || (y + 1 == kH && x_in && z_in);
        if (can_look_up && block_at(x, y + 1, z) == id) return 1.0f;
        const uint8_t meta = meta_at(x, y, z);
        int amount;
        if (meta & Block_Meta::fluid_falling_bit) amount = 8;
        else {
            const int level = meta & Block_Meta::fluid_level_mask;
            amount = level == 0 ? 8 : 8 - level;
        }
        return static_cast<float>(amount) / 9.0f;
    };

    for (int section_index = 0; section_index < Config::chunk_section_count; ++section_index) {
        Chunk_Section_Range& range = meshes.sections[section_index];
        range = Chunk_Section_Range{};
        range.solid_start = meshes.solid.size();
        range.flora_start = meshes.flora.size();
        range.liquid_start = meshes.liquid.size();

        const int y_begin = section_index * Config::chunk_section_height;
        const int y_end = y_begin + Config::chunk_section_height;

        if (!all_air) {
            // --- Непрозрачная геометрия: жадное слияние по шести направлениям ---
            for (int axis = 0; axis < 3; ++axis) {
                for (int dir = 0; dir < 2; ++dir) {
                    const int sign = dir == 0 ? 1 : -1;
                    if (sign < 0 && only_positive_faces) continue;

                    const int ua = kAxisU[axis];
                    const int va = kAxisV[axis];
                    // Диапазоны по оси среза и по оси v: по Y ограничены текущей секцией.
                    const int s_begin = axis == 1 ? y_begin : 0;
                    const int s_end = axis == 1 ? y_end : kN;
                    const int v_begin = va == 1 ? y_begin : 0;
                    const int v_end = va == 1 ? y_end : kN;
                    const int v_count = v_end - v_begin;
                    const int face = axis * 2 + (sign > 0 ? 0 : 1);

                    const Mesh_Tables& tab = mesh_tables();
                    constexpr int stride[3] = {1, static_cast<int>(kLayer), kN};  // x, y, z в плоском массиве
                    const int s_stride = stride[axis], u_stride = stride[ua], v_stride = stride[va];

                    for (int s = s_begin; s < s_end; ++s) {
                        if (axis == 1 && layer_block_count[s] == 0) continue;
                        // Сосед по оси среза лежит в этом же чанке (быстрый путь: индекс + шаг)
                        // или за его границей (редкий путь через кэш соседей).
                        const int ns = s + sign;
                        const bool inside = ns >= 0 && ns < kN;
                        bool any = false;
                        for (int v = 0; v < v_count; ++v) {
                            const int row = s * s_stride + (v_begin + v) * v_stride;
                            for (int u = 0; u < kN; ++u) {
                                uint32_t key = 0;
                                const int idx = row + u * u_stride;
                                const uint8_t cur = S.blocks[idx];
                                if (tab.greedy[cur]) {
                                    uint8_t neighbor;
                                    int c[3];
                                    if (inside) {
                                        neighbor = S.blocks[idx + sign * s_stride];
                                    } else {
                                        c[axis] = s; c[ua] = u; c[va] = v_begin + v;
                                        c[axis] += sign;
                                        neighbor = block_at(c[0], c[1], c[2]);
                                    }
                                    if (tab.transparent[neighbor] &&
                                        !(neighbor == cur && tab.cull_same[cur])) {
                                        c[axis] = s; c[ua] = u; c[va] = v_begin + v;
                                        uint8_t meta = 0;
                                        bool rotate = false;
                                        if (tab.uses_meta[cur]) {
                                            meta = blocks.get_meta(c[0], c[1], c[2]);
                                            props[cur]->resolve_face_uv(face, meta, rotate);
                                        }
                                        uint8_t face_light = inside
                                            ? S.light[idx + sign * s_stride]
                                            : light_at(c[0] + (axis == 0 ? sign : 0),
                                                       c[1] + (axis == 1 ? sign : 0),
                                                       c[2] + (axis == 2 ? sign : 0));
                                        if (tab.cull_same[cur]) face_light = tab.light_coarse[face_light];
                                        key = kKeyPresent | (rotate ? kKeyRotate : 0u) |
                                              static_cast<uint32_t>(cur) |
                                              (static_cast<uint32_t>(meta) << 8) |
                                              (static_cast<uint32_t>(face_light) << 16);
                                        any = true;
                                    }
                                }
                                S.mask[static_cast<size_t>(v) * kN + u] = key;
                            }
                        }
                        if (!any) continue;

                        for (int v = 0; v < v_count; ++v) {
                            for (int u = 0; u < kN;) {
                                const uint32_t key = S.mask[static_cast<size_t>(v) * kN + u];
                                if (key == 0) { ++u; continue; }

                                int w = 1, h = 1;
                                if (!(key & kKeyRotate)) {
                                    while (u + w < kN &&
                                           S.mask[static_cast<size_t>(v) * kN + u + w] == key) ++w;
                                    bool grow = true;
                                    while (v + h < v_count && grow) {
                                        for (int i = 0; i < w; ++i) {
                                            if (S.mask[static_cast<size_t>(v + h) * kN + u + i] != key) {
                                                grow = false;
                                                break;
                                            }
                                        }
                                        if (grow) ++h;
                                    }
                                }
                                for (int dv = 0; dv < h; ++dv) {
                                    for (int du = 0; du < w; ++du) {
                                        S.mask[static_cast<size_t>(v + dv) * kN + u + du] = 0;
                                    }
                                }

                                int c[3];
                                c[axis] = s; c[ua] = u; c[va] = v_begin + v;
                                const Block_Properties& bp = *props[key & 0xFFu];
                                const uint8_t meta = static_cast<uint8_t>((key >> 8) & 0xFFu);
                                bool rotate = false;
                                const glm::vec4& uv = bp.uses_meta
                                    ? bp.resolve_face_uv(face, meta, rotate)
                                    : bp.get_uv(axis == 1 ? sign : 0);
                                emit_terrain_quad(meshes.solid,
                                                  static_cast<float>(base_x + c[0]),
                                                  static_cast<float>(base_y + c[1]),
                                                  static_cast<float>(base_z + c[2]),
                                                  axis, sign, w, h, uv,
                                                  static_cast<float>((key >> 16) & 0xFFu), rotate);
                                u += w;
                            }
                        }
                    }
                }
            }

            // --- Флора и жидкость: по одной клетке ---
            for (int y = y_begin; y < y_end; ++y) {
                if (layer_block_count[y] == 0) continue;
                for (int z = 0; z < kN; ++z) {
                    for (int x = 0; x < kN; ++x) {
                        const uint8_t id = S.blocks[(static_cast<size_t>(y) * kN + z) * kN + x];
                        if (id == 0) continue;
                        const Block_Properties& bp = *props[id];
                        const Block_Types block = static_cast<Block_Types>(id);

                        if (bp.mesh_style == Block_Mesh_Style::XStyle) {
                            const float own_light = static_cast<float>(
                                S.light[(static_cast<size_t>(y) * kN + z) * kN + x]);
                            add_flora_block_data(meshes, chunk_x, chunk_y, chunk_z, x, y, z, block, own_light);
                            continue;
                        }
                        if (bp.state != Block_State::Liquid) continue;

                        // Высота поверхности клетки: источник — 8/9 блока, течение — меньше
                        // (уровень лежит в метаданных блока), с жидкостью над собой — полный блок.
                        const float h = liquid_height(x, y, z, id);

                        static constexpr int dirs[6][3] = {
                            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
                        };
                        for (int d = 0; d < 6; ++d) {
                            const bool negative = (d & 1) != 0;
                            if (negative && only_positive_faces) continue;
                            const int nx = x + dirs[d][0], ny = y + dirs[d][1], nz = z + dirs[d][2];
                            const uint8_t nid = block_at(nx, ny, nz);

                            float ylo = 0.0f, yhi = h;
                            if (nid == id) {
                                // Та же жидкость рядом: боковая грань нужна только как "ступенька" над
                                // более низким соседом; вверх/вниз внутри жидкости граней нет.
                                if (dirs[d][1] != 0) continue;
                                const float hn = liquid_height(nx, ny, nz, nid);
                                if (hn >= h - 1e-4f) continue;
                                ylo = hn;
                            } else {
                                // Грань видна, только если с той стороны воздух или прозрачный
                                // (не жидкий) блок: стенки против камня и дна не рисуем.
                                if (props[nid]->state == Block_State::Liquid ||
                                    !props[nid]->is_transparent) continue;
                            }
                            add_liquid_face_data(meshes, chunk_x, chunk_y, chunk_z, x, y, z,
                                                 dirs[d][0], dirs[d][1], dirs[d][2], block,
                                                 static_cast<float>(light_at(nx, ny, nz)), ylo, yhi);
                        }
                    }
                }
            }
        }

        range.solid_count = meshes.solid.size() - range.solid_start;
        range.flora_count = meshes.flora.size() - range.flora_start;
        range.liquid_count = meshes.liquid.size() - range.liquid_start;
        range.has_geometry = (range.solid_count + range.flora_count + range.liquid_count) > 0;

        if (range.has_geometry) {
            range.min_y = std::numeric_limits<float>::max();
            range.max_y = std::numeric_limits<float>::lowest();
            const auto expand = [&range](const auto& verts, size_t start, size_t count) {
                for (size_t i = start; i < start + count; ++i) {
                    range.min_y = std::min(range.min_y, verts[i].y);
                    range.max_y = std::max(range.max_y, verts[i].y);
                }
            };
            expand(meshes.solid, range.solid_start, range.solid_count);
            expand(meshes.flora, range.flora_start, range.flora_count);
            expand(meshes.liquid, range.liquid_start, range.liquid_count);
        } else {
            range.min_y = static_cast<float>(base_y + y_begin);
            range.max_y = range.min_y;
        }
    }

    return meshes;
}

void Chunk::build_mesh() {
    if (!m_texture_atlas) {
        LOG_ERROR("Chunk::build_mesh: Texture_Atlas is nullptr");
        return;
    }

    const Chunk_Manager* manager = m_chunk_manager;
    Chunk_Neighbor_Lookup lookup;
    Chunk_Light_Lookup light_lookup;
    if (manager) {
        lookup = [manager](int wx, int wy, int wz) { return manager->get_block_world(wx, wy, wz); };
        light_lookup = [manager](int wx, int wy, int wz) { return manager->get_light_world(wx, wy, wz); };
    }
    Chunk_Meta_Lookup meta_lookup;
    if (manager) {
        meta_lookup = [manager](int wx, int wy, int wz) { return manager->get_block_meta_world(wx, wy, wz); };
    }

    m_meshes = build_chunk_mesh(m_blocks, m_block_light, m_chunk_x, m_chunk_y, m_chunk_z,
                                lookup, light_lookup, *m_texture_atlas, false, meta_lookup);
    m_is_mesh_dirty = false;
    m_mesh_version = ++g_mesh_version_counter;
}

void Chunk::apply_mesh(Chunk_Meshes meshes) {
    m_meshes = std::move(meshes);
    m_is_mesh_dirty = false;
    m_mesh_version = ++g_mesh_version_counter;
}
bool Chunk::is_empty() const {
    return m_meshes.solid.empty() && m_meshes.flora.empty() && m_meshes.liquid.empty();
}

Block_Types Chunk::get_block(int x, int y, int z) const {
    if (x < 0 || x >= Config::chunk_size ||
        y < 0 || y >= Config::chunk_height ||
        z < 0 || z >= Config::chunk_size) {
        return Block_Types::Air;
    }
    return m_blocks.get(x, y, z);
}

uint8_t Chunk::get_meta(int x, int y, int z) const {
    if (x < 0 || x >= Config::chunk_size ||
        y < 0 || y >= Config::chunk_height ||
        z < 0 || z >= Config::chunk_size) {
        return 0;
    }
    return m_blocks.get_meta(x, y, z);
}

void Chunk::set_meta(int x, int y, int z, uint8_t value) {
    if (x < 0 || x >= Config::chunk_size ||
        y < 0 || y >= Config::chunk_height ||
        z < 0 || z >= Config::chunk_size) {
        return;
    }
    m_blocks.set_meta(x, y, z, value);
    m_is_mesh_dirty = true;
    m_is_modified = true;
}

void Chunk::set_block(int x, int y, int z, Block_Types type) {
    if (x < 0 || x >= Config::chunk_size ||
        y < 0 || y >= Config::chunk_height ||
        z < 0 || z >= Config::chunk_size) {
        return;
    }

    const Block_Types slot = m_blocks.get(x, y, z);
    const bool was_air = (slot == Block_Types::Air);
    const bool now_air = (type == Block_Types::Air);
    if (was_air && !now_air) ++m_layer_block_count[y];
    else if (!was_air && now_air) --m_layer_block_count[y];

    m_blocks.set(x, y, z, type);
    m_is_mesh_dirty = true;
    m_is_modified = true;
}

uint8_t Chunk::get_light(int x, int y, int z) const {
    if (x < 0 || x >= Config::chunk_size ||
        y < 0 || y >= Config::chunk_height ||
        z < 0 || z >= Config::chunk_size) {
        return 0;
    }
    return m_block_light.get_block(x, y, z);
}

void Chunk::set_light(int x, int y, int z, uint8_t level) {
    if (x < 0 || x >= Config::chunk_size ||
        y < 0 || y >= Config::chunk_height ||
        z < 0 || z >= Config::chunk_size) {
        return;
    }
    // Не трогаем m_is_mesh_dirty здесь: Chunk_Manager::propagate_light правит свет пачкой
    // (много клеток за раз, часто в нескольких чанках) и сам решает, когда и какие чанки
    // пометить на пересборку меша — иначе один вызов set_light пометил бы чанк грязным
    // посреди ещё не завершённого BFS-прохода, что бессмысленно.
    m_block_light.set_block(x, y, z, level);
}

glm::vec3 Chunk::get_world_position(int local_x, int local_y, int local_z) const {
    return glm::vec3(
        static_cast<float>(m_chunk_x * Config::chunk_size + local_x),
        static_cast<float>(m_chunk_y * Config::chunk_height + local_y),
        static_cast<float>(m_chunk_z * Config::chunk_size + local_z)
    );
}
