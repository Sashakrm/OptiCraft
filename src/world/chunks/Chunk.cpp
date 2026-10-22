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
#include <map>

namespace {
    std::atomic<uint64_t> g_mesh_version_counter{0};
    // Номер объекта чанка (см. Chunk::get_instance_id). Начинается с 1: 0 значит «не привязано к объекту».
    std::atomic<uint64_t> g_chunk_instance_counter{0};
    // Счётчик изменений связности (см. current_connectivity_epoch).
    std::atomic<uint64_t> g_connectivity_epoch{0};

    // Меняется ли связность секций при замене мешей: сравниваем всё, что читает обход достижимости.
    bool same_connectivity(const Chunk_Meshes& a, const Chunk_Meshes& b) {
        for (size_t i = 0; i < a.sections.size(); ++i) {
            const Section_Connectivity& x = a.sections[i].conn;
            const Section_Connectivity& y = b.sections[i].conn;
            if (x.kind != y.kind || x.comp_count != y.comp_count || x.face_begin != y.face_begin ||
                x.face_comps != y.face_comps || x.masks != y.masks) {
                return false;
            }
        }
        return true;
    }
}

uint64_t current_connectivity_epoch() {
    return g_connectivity_epoch.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
//  Chunk_Block_Grid: палитра + упакованные номера (описание формата — в Chunk.h)
// ---------------------------------------------------------------------------
void Chunk_Block_Grid::apply_bits(uint8_t bits) {
    m_bits = bits;
    m_log_bits = bits == 16 ? 4 : bits == 8 ? 3 : bits == 4 ? 2 : bits == 2 ? 1 : 0;
    m_word_shift = static_cast<uint8_t>(6 - m_log_bits);
    m_slot_mask = static_cast<uint8_t>((1u << m_word_shift) - 1u);
    m_value_mask = bits == 0 ? 0u : (1u << bits) - 1u;
}

void Chunk_Block_Grid::repack(uint8_t new_bits, const uint16_t* remap) {
    std::vector<uint16_t> indices(voxel_count);
    for (size_t i = 0; i < voxel_count; ++i) {
        const uint16_t old_index = index_at(i);
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

uint16_t Chunk_Block_Grid::palette_index_for(Block_ID block_id) {
    for (size_t i = 0; i < m_palette.size(); ++i) {
        if (m_palette[i] == block_id) return static_cast<uint16_t>(i);
    }
    // Запись, которую больше никто не использует (блок целиком заменили), можно занять
    // повторно — так палитра не растёт при активной правке чанка.
    for (size_t i = 0; i < m_palette.size(); ++i) {
        if (m_counts[i] == 0) {
            m_palette[i] = block_id;
            return static_cast<uint16_t>(i);
        }
    }
    // Палитра переполнилась для текущей ширины — расширяем её (1 -> 2 -> 4 -> 8 -> 16 бит).
    if (m_palette.size() >= capacity_for_bits(m_bits)) {
        repack(bits_for_palette(m_palette.size() + 1));
    }
    m_palette.push_back(block_id);
    m_counts.push_back(0);
    return static_cast<uint16_t>(m_palette.size() - 1);
}

void Chunk_Block_Grid::assign_flat(const Block_ID* flat) {
    // Таблица "id -> номер в новой палитре" живёт в thread_local: заполнять 65536 записей на
    // каждый чанк было бы дорого, поэтому после работы возвращаем занятые слоты в исходное состояние.
    thread_local std::vector<uint32_t> slot_of(k_max_block_types, 0xFFFFFFFFu);

    m_palette.clear();
    m_counts.clear();
    for (size_t i = 0; i < voxel_count; ++i) {
        const Block_ID id = flat[i];
        if (slot_of[id] == 0xFFFFFFFFu) {
            slot_of[id] = static_cast<uint32_t>(m_palette.size());
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
    } else {
        const size_t per_word = static_cast<size_t>(1) << m_word_shift;
        m_words.assign(voxel_count / per_word, 0);
        for (size_t w = 0; w < m_words.size(); ++w) {
            uint64_t packed = 0;
            const Block_ID* src = flat + w * per_word;
            for (size_t k = 0; k < per_word; ++k) {
                packed |= static_cast<uint64_t>(slot_of[src[k]]) << (k * m_bits);
            }
            m_words[w] = packed;
        }
    }

    for (const Block_ID id : m_palette) slot_of[id] = 0xFFFFFFFFu;
}

void Chunk_Block_Grid::unpack(Block_ID* out) const {
    if (m_bits == 0) {
        std::fill_n(out, voxel_count, m_palette[0]);
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

    std::vector<uint16_t> remap(m_palette.size(), 0);
    std::vector<Block_ID> new_palette;
    std::vector<uint16_t> new_counts;
    for (size_t i = 0; i < m_palette.size(); ++i) {
        if (m_counts[i] == 0) continue;
        remap[i] = static_cast<uint16_t>(new_palette.size());
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
    // Номер объекта нужен потому, что позиция в мире не отличает объект от его замены: выгрузили
    // и загрузили заново — позиция та же, а результат задачи, снятой для старого объекта, применять нельзя.
    m_instance_id = ++g_chunk_instance_counter;
    m_dimension = generator.get_dimension_id();
}

void Chunk::recalculate_metadata() {
    m_layer_block_count.fill(0);
    if (m_blocks.is_uniform()) {
        if (m_blocks.uniform_block() != Block_Types::Air) {
            m_layer_block_count.fill(Config::chunk_size * Config::chunk_size);
        }
        return;
    }
    std::vector<Block_ID> flat(Chunk_Block_Grid::voxel_count);
    m_blocks.unpack(flat.data());
    constexpr size_t layer = static_cast<size_t>(Config::chunk_size) * Config::chunk_size;
    const Block_ID air = static_cast<Block_ID>(Block_Types::Air);
    for (int y = 0; y < Config::chunk_height; ++y) {
        const Block_ID* row = flat.data() + static_cast<size_t>(y) * layer;
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
    constexpr uint64_t kKeyPresent = 1ull << 40;
    constexpr uint64_t kKeyRotate = 1ull << 41;   // повёрнутая текстура — такой квад не сливается

    // Свойства блока по id: реестр неизменен после загрузки blocks.json, а Block_Properties —
    // тяжёлая структура, поэтому в горячем цикле держим только указатели.
    const std::vector<const Block_Properties*>& props_table() {
        static const std::vector<const Block_Properties*> table = [] {
            std::vector<const Block_Properties*> t(k_max_block_types, nullptr);
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
        std::vector<uint8_t> greedy = std::vector<uint8_t>(k_max_block_types, 0);      // твёрдый блок, сливается жадным мешером
        std::vector<uint8_t> transparent = std::vector<uint8_t>(k_max_block_types, 0); // грань к такому соседу видна
        std::vector<uint8_t> uses_meta = std::vector<uint8_t>(k_max_block_types, 0);
        // Полупрозрачный кубический блок (листва, стекло, лёд, слизь): грань к такому же блоку
        // не рисуется (внутри кроны/глыбы граней нет), а свет грани огрубляется (light_coarse),
        // чтобы соседние грани сливались в большие квады, а не дробились из-за 1 уровня света.
        std::vector<uint8_t> cull_same = std::vector<uint8_t>(k_max_block_types, 0);
        // Непрозрачный полный куб (жадно-мешаемый и не прозрачный): такой блок закрывает AO соседних углов.
        std::vector<uint8_t> occludes = std::vector<uint8_t>(k_max_block_types, 0);
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
                m.occludes[i] = (m.greedy[i] && !p.is_transparent) ? 1 : 0;
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
        std::array<Block_ID, kVoxels> blocks;      // id блоков чанка, плоско (y, z, x)
        std::array<uint8_t, kVoxels> light;        // упакованный свет, плоско
        std::array<uint8_t, kVoxels> meta;         // метаданные блоков, плоско (для GPU-нагрузки)
        // Соседние клетки за границей чанка запрашиваются лениво и кэшируются: за пределами
        // загруженного мира блок спрашивается у генератора (дорого), поэтому берём только те
        // клетки, которые реально понадобились.
        std::array<Block_ID, 6 * kLayer> border_block;
        std::array<uint8_t, 6 * kLayer> border_light;
        std::array<uint8_t, 6 * kLayer> block_known, light_known;
        // Кольцо рёбер и углов за границей (для AO), лениво, как и грани.
        std::array<Block_ID, Chunk_Ring::kSize> ring_block;
        std::array<uint8_t, Chunk_Ring::kSize> ring_known;
        std::array<uint64_t, kLayer> mask;   // ключ слияния граней, см. kKey* ниже
        // Обход компонент прозрачных клеток одной секции (см. compute_section_connectivity).
        std::array<uint8_t, kLayer * Config::chunk_section_height> section_seen;
        std::array<uint16_t, kLayer * Config::chunk_section_height> section_stack;
        std::array<uint16_t, kLayer * Config::chunk_section_height> section_comp;   // компонента клетки
        std::array<uint16_t, kLayer * Config::chunk_section_height> section_final;  // компонента -> номер граничной
        std::array<uint8_t, Section_Boundary::kCount> boundary_label;             // номер граничной компоненты по позиции
        std::array<uint64_t, Section_Boundary::kMaxComps * 16> face_bits;         // маски одной грани: компонента * слова
    };

    Mesh_Scratch& mesh_scratch() {
        thread_local std::unique_ptr<Mesh_Scratch> scratch = std::make_unique<Mesh_Scratch>();
        return *scratch;
    }

    // Связность граничных клеток секции (см. Section_Connectivity). Клетка проводит обзор, если она не
    // полный непрозрачный куб (Mesh_Tables::occludes): воздух, вода, стекло, листва, флора.
    // Проход 1: компоненты всех прозрачных клеток секции (номер — порядок обнаружения).
    // Проход 2: граничные позиции получают номера компонент, которые касаются границы; внутренние — нет.
    // Проход 3: для каждой грани — маски позиций её компонент (бит — локальная позиция грани, см. kBase).
    // Блоки секции лежат в S.blocks с y_begin, порядок — как в build_chunk_mesh (y, z, x).
    Section_Connectivity compute_section_connectivity(Mesh_Scratch& S, int y_begin) {
        constexpr int kSecH = Config::chunk_section_height;
        constexpr int kLayerCells = kN * kN;
        constexpr int kCells = kSecH * kLayerCells;
        const size_t base = static_cast<size_t>(y_begin) * kLayerCells;
        const Mesh_Tables& tab = mesh_tables();
        const auto passable = [&](int local) {
            return tab.occludes[S.blocks[base + static_cast<size_t>(local)]] == 0;
        };

        std::fill(S.section_seen.begin(), S.section_seen.end(), static_cast<uint8_t>(0));
        int raw_count = 0;
        for (int seed = 0; seed < kCells; ++seed) {
            if (S.section_seen[seed]) continue;
            S.section_seen[seed] = 1;
            if (!passable(seed)) continue;

            const uint16_t id = static_cast<uint16_t>(raw_count++);
            S.section_comp[seed] = id;
            size_t top = 0;
            S.section_stack[top++] = static_cast<uint16_t>(seed);
            while (top > 0) {
                const int li = S.section_stack[--top];
                const int x = li % kN;
                const int z = (li / kN) % kN;
                const int ly = li / kLayerCells;
                const auto visit = [&](int next) {
                    if (S.section_seen[next]) return;
                    S.section_seen[next] = 1;
                    if (!passable(next)) return;
                    S.section_comp[next] = id;
                    S.section_stack[top++] = static_cast<uint16_t>(next);
                };
                if (x > 0) visit(li - 1);
                if (x < kN - 1) visit(li + 1);
                if (z > 0) visit(li - kN);
                if (z < kN - 1) visit(li + kN);
                if (ly > 0) visit(li - kLayerCells);
                if (ly < kSecH - 1) visit(li + kLayerCells);
            }
        }

        // Граничные позиции -> клетка секции -> компонента -> номер граничной компоненты.
        Section_Connectivity out;
        std::fill(S.section_final.begin(), S.section_final.begin() + raw_count, static_cast<uint16_t>(0xFFFF));
        int comp_count = 0;
        int passable_boundary = 0;
        for (int p = 0; p < Section_Boundary::kCount; ++p) {
            const int f = Section_Boundary::face_of(p);
            const int q = p - Section_Boundary::kBase[f];
            int lx = 0, ly = 0, lz = 0;
            switch (f) {
                case 0: lx = kN - 1; lz = q / kSecH; ly = q % kSecH; break;  // +X: (z, y)
                case 1: lx = 0;      lz = q / kSecH; ly = q % kSecH; break;  // -X
                case 2: ly = kSecH - 1; lz = q / kN; lx = q % kN; break;     // +Y: (z, x)
                case 3: ly = 0;         lz = q / kN; lx = q % kN; break;     // -Y
                case 4: lz = kN - 1; lx = q / kSecH; ly = q % kSecH; break;  // +Z: (x, y)
                default: lz = 0;     lx = q / kSecH; ly = q % kSecH; break;  // -Z
            }
            const int li = (ly * kN + lz) * kN + lx;
            if (!passable(li)) {
                S.boundary_label[p] = Section_Boundary::kNoComp;
                continue;
            }
            ++passable_boundary;
            const uint16_t raw = S.section_comp[li];
            if (S.section_final[raw] == 0xFFFF) {
                S.section_final[raw] = static_cast<uint16_t>(comp_count++);
            }
            S.boundary_label[p] = static_cast<uint8_t>(S.section_final[raw]);
        }

        if (passable_boundary == 0) {
            out.kind = Section_Connectivity::Kind::Closed;
            return out;
        }
        // Одна компонента на всю границу, все её позиции проходимы: секция видна насквозь. Слишком много
        // компонент (больше 255) тоже сводим к Open: это грубее, но безопасно (лишние секции, а не пропуски).
        if (comp_count > Section_Boundary::kMaxComps ||
            (comp_count == 1 && passable_boundary == Section_Boundary::kCount)) {
            out.kind = Section_Connectivity::Kind::Open;
            return out;
        }

        // Маски по граням: компонента грани — запись с номером и смещением маски; маски идут в порядке записей.
        out.kind = Section_Connectivity::Kind::Mixed;
        out.comp_count = static_cast<uint16_t>(comp_count);
        for (int f = 0; f < 6; ++f) {
            out.face_begin[f] = static_cast<uint16_t>(out.face_comps.size());
            const int words = Section_Boundary::kFaceWords[f];
            const int face_size = words * 64;
            const int first = Section_Boundary::kBase[f];
            std::fill(S.face_bits.begin(), S.face_bits.begin() + static_cast<size_t>(comp_count) * words, uint64_t{0});
            std::array<bool, Section_Boundary::kMaxComps> touched{};
            for (int i = 0; i < face_size; ++i) {
                const uint8_t l = S.boundary_label[first + i];
                if (l == Section_Boundary::kNoComp) continue;
                S.face_bits[static_cast<size_t>(l) * words + static_cast<size_t>(i >> 6)] |= uint64_t{1} << (i & 63);
                touched[l] = true;
            }
            for (int l = 0; l < comp_count; ++l) {
                if (!touched[l]) continue;
                const size_t at = static_cast<size_t>(l) * words;
                out.face_comps.push_back({static_cast<uint8_t>(l), static_cast<uint32_t>(out.masks.size())});
                out.masks.insert(out.masks.end(), S.face_bits.begin() + static_cast<std::ptrdiff_t>(at),
                                 S.face_bits.begin() + static_cast<std::ptrdiff_t>(at) + words);
            }
        }
        out.face_begin[6] = static_cast<uint16_t>(out.face_comps.size());
        return out;
    }

    // Индекс клетки за границей чанка в кэше соседей (грань: -X,+X,-Y,+Y,-Z,+Z).
    inline size_t border_slot(int x, int y, int z) {
        if (x < 0 || x >= kN) return (x >= kN ? 1 : 0) * kLayer + static_cast<size_t>(y) * kN + z;
        if (y < 0 || y >= kH) return (y >= kH ? 3 : 2) * kLayer + static_cast<size_t>(z) * kN + x;
        return (z >= kN ? 5 : 4) * kLayer + static_cast<size_t>(y) * kN + x;
    }

    // Порядок вершин квада (та же раскладка, что в terrain_gpu_vertex.glsl). Код угла c = fu + 2*fv,
    // вариант = axis*2 + (знак > 0 ? 0 : 1), по шесть вершин на вариант (два треугольника).
    // kCornerFlip — тот же квад, разрезанный по другой диагонали, с той же ориентацией обхода.
    constexpr int kCorner[36] = {
        0, 2, 3, 0, 3, 1,
        0, 3, 2, 0, 1, 3,
        0, 3, 1, 0, 2, 3,
        0, 1, 3, 0, 3, 2,
        0, 1, 3, 0, 3, 2,
        0, 3, 1, 0, 2, 3};
    constexpr int kCornerFlip[36] = {
        0, 2, 1, 1, 2, 3,
        0, 1, 2, 1, 3, 2,
        0, 2, 1, 1, 2, 3,
        0, 1, 2, 1, 3, 2,
        0, 1, 2, 1, 3, 2,
        0, 2, 1, 1, 2, 3};

    // Диагональ квада идёт через более тёмную пару углов (сумма AO по ней меньше). Иначе квад с одним
    // тёмным углом получает резкую полосу по диагонали. Правило совпадает с GPU-шейдером.
    bool flip_quad_diagonal(uint32_t ao_code) {
        const uint32_t a0 = ao_code & 3u;
        const uint32_t a1 = (ao_code >> 2) & 3u;
        const uint32_t a2 = (ao_code >> 4) & 3u;
        const uint32_t a3 = (ao_code >> 6) & 3u;
        return a0 + a3 > a1 + a2;
    }

    // Один квад непрозрачной геометрии. w — размер вдоль оси u, h — вдоль оси v (см. kAxisU/kAxisV
    // ниже), (wx, wy, wz) — мировые координаты угла клетки с минимальными координатами. ao_code —
    // AO четырёх углов (два бита на угол). Вершины и UV строятся по тем же правилам, что в GPU-пути.
    void emit_terrain_quad(std::vector<Terrain_Vertex>& out,
                           float wx, float wy, float wz,
                           int axis, int sign, int w, int h,
                           const glm::vec4& uv, float light, bool rotate, uint32_t ao_code) {
        const float ny = axis == 1 ? static_cast<float>(sign) : 0.0f;
        const float fw = static_cast<float>(w);
        const float fh = static_cast<float>(h);
        const float plane = sign > 0 ? 1.0f : 0.0f;
        const int variant = axis * 2 + (sign > 0 ? 0 : 1);
        const int* order = flip_quad_diagonal(ao_code) ? kCornerFlip : kCorner;
        for (int k = 0; k < 6; ++k) {
            const int code = order[variant * 6 + k];
            const float fu = static_cast<float>(code & 1);
            const float fv = static_cast<float>((code >> 1) & 1);
            float x, y, z, lu, lv;
            if (axis == 0) {            // грань ±X: u вдоль Z, v вдоль Y
                x = wx + plane; y = wy + fv * fh; z = wz + fu * fw;
                lu = fu * fw;       lv = fv * fh;
            } else if (axis == 1) {     // грань ±Y: u вдоль X, v вдоль Z
                x = wx + fu * fw; y = wy + plane; z = wz + fv * fh;
                lu = fu * fw;       lv = (1.0f - fv) * fh;
            } else {                    // грань ±Z: u вдоль X, v вдоль Y
                x = wx + fu * fw; y = wy + fv * fh; z = wz + plane;
                lu = fu * fw;       lv = fv * fh;
            }
            if (rotate) {               // поворот текстуры на 90°: (a, b) -> (b, 1 - a)
                const float a = lu;
                lu = lv;
                lv = 1.0f - a;
            }
            const float ao = static_cast<float>((ao_code >> (2 * code)) & 3u);
            out.push_back({x, y, z, lu, lv, ny, light, uv.x, uv.y, uv.z, uv.w, ao});
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
                              const Chunk_Meta_Lookup& meta_lookup,
                              bool solid_on_gpu) {
    // UV блоков резолвятся один раз при загрузке blocks.json (см. Block_Registry::
    // load_from_file → resolve_sprite_uv) и лежат готовыми в Block_Properties, так что atlas
    // для самой геометрии не нужен; параметр оставлен как "атлас обязан быть загружен".
    (void)atlas;
    Chunk_Meshes meshes;
    meshes.solid_on_gpu = solid_on_gpu;

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
        std::fill(S.ring_known.begin(), S.ring_known.end(), static_cast<uint8_t>(0));

        // Layer culling: слои без единого блока пропускаются (дёшево — 32x32 на слой).
        for (int y = 0; y < kH; ++y) {
            const Block_ID* row = S.blocks.data() + static_cast<size_t>(y) * kLayer;
            int count = 0;
            for (size_t i = 0; i < kLayer; ++i) count += (row[i] != 0);
            layer_block_count[y] = count;
        }
    }

    // Блок / свет в клетке чанка или в соседней клетке за его границей (ровно одна координата
    // выходит за диапазон: диагональных соседей мешер не спрашивает).
    const auto block_at = [&](int x, int y, int z) -> Block_ID {
        if (static_cast<unsigned>(x) < static_cast<unsigned>(kN) &&
            static_cast<unsigned>(y) < static_cast<unsigned>(kH) &&
            static_cast<unsigned>(z) < static_cast<unsigned>(kN)) {
            return S.blocks[(static_cast<size_t>(y) * kN + z) * kN + x];
        }
        const size_t slot = border_slot(x, y, z);
        if (!S.block_known[slot]) {
            S.block_known[slot] = 1;
            S.border_block[slot] = neighbor_lookup
                ? static_cast<Block_ID>((
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

    // Рёбра и углы за границей (две или три координаты вне чанка): лениво, через лукап соседей.
    const auto ring_block_at = [&](int x, int y, int z) -> Block_ID {
        const int idx = chunk_ring_index(x, y, z);
        if (!S.ring_known[idx]) {
            S.ring_known[idx] = 1;
            S.ring_block[idx] = neighbor_lookup
                ? static_cast<Block_ID>((
                      neighbor_lookup(base_x + x, base_y + y, base_z + z)))
                : static_cast<uint8_t>(0);
        }
        return S.ring_block[idx];
    };

    // Закрывает ли клетка свет AO: только полный непрозрачный куб (см. Mesh_Tables::occludes).
    const auto occludes_at = [&](int x, int y, int z) -> bool {
        if (static_cast<unsigned>(x) < static_cast<unsigned>(kN) &&
            static_cast<unsigned>(y) < static_cast<unsigned>(kH) &&
            static_cast<unsigned>(z) < static_cast<unsigned>(kN)) {
            return mesh_tables().occludes[S.blocks[(static_cast<size_t>(y) * kN + z) * kN + x]] != 0;
        }
        const int out_axes = int(x < 0 || x >= kN) + int(y < 0 || y >= kH) + int(z < 0 || z >= kN);
        const Block_ID id = out_axes >= 2 ? ring_block_at(x, y, z) : block_at(x, y, z);
        return mesh_tables().occludes[id] != 0;
    };

    // Вершинный AO четырёх углов грани клетки c с нормалью по оси axis (знак sign). Угол fu + 2*fv
    // получает уровень 0..3 по двум боковым клеткам слоя перед гранью и по диагональной клетке:
    // 3 — открыт, 0 — закрыт двумя стенками. Два бита на угол, всего 8 бит на клетку.
    const auto face_ao = [&](const int c[3], int axis, int sign, int ua, int va) -> uint32_t {
        int base[3] = {c[0], c[1], c[2]};
        base[axis] += sign;
        uint32_t code = 0;
        for (int fv = 0; fv < 2; ++fv) {
            for (int fu = 0; fu < 2; ++fu) {
                const int du = fu ? 1 : -1;
                const int dv = fv ? 1 : -1;
                int p1[3] = {base[0], base[1], base[2]};
                int p2[3] = {base[0], base[1], base[2]};
                int p3[3] = {base[0], base[1], base[2]};
                p1[ua] += du;
                p2[va] += dv;
                p3[ua] += du;
                p3[va] += dv;
                const bool s1 = occludes_at(p1[0], p1[1], p1[2]);
                const bool s2 = occludes_at(p2[0], p2[1], p2[2]);
                const bool s3 = occludes_at(p3[0], p3[1], p3[2]);
                const uint32_t level = (s1 && s2)
                    ? 0u
                    : 3u - (static_cast<uint32_t>(s1) + static_cast<uint32_t>(s2) + static_cast<uint32_t>(s3));
                code |= level << (2 * (fu + 2 * fv));
            }
        }
        return code;
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
    const auto liquid_height = [&](int x, int y, int z, Block_ID id) -> float {
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
        // Связность для отсечения укрытий. Воздух целиком проводит обзор: секция остаётся Open (по умолчанию).
        if (!all_air) range.conn = compute_section_connectivity(S, y_begin);

        // GPU: есть ли в секции жадно-мешаемый блок. Тогда секция рисуется GPU-квадами (их
        // число на CPU неизвестно), и её Y-диапазон — это весь диапазон секции.
        bool section_greedy = false;
        if (solid_on_gpu && !all_air) {
            for (int y = y_begin; y < y_end && !section_greedy; ++y) {
                if (layer_block_count[y] == 0) continue;
                const Block_ID* row = S.blocks.data() + static_cast<size_t>(y) * kLayer;
                for (size_t i = 0; i < kLayer; ++i) {
                    if (mesh_tables().greedy[row[i]]) { section_greedy = true; break; }
                }
            }
        }

        if (!all_air && !solid_on_gpu) {
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
                                uint64_t key = 0;
                                const int idx = row + u * u_stride;
                                const Block_ID cur = S.blocks[idx];
                                if (tab.greedy[cur]) {
                                    Block_ID neighbor;
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
                                        if (tab.uses_meta[cur]) {
                                            meta = blocks.get_meta(c[0], c[1], c[2]);
                                        }
                                        uint8_t face_light = inside
                                            ? S.light[idx + sign * s_stride]
                                            : light_at(c[0] + (axis == 0 ? sign : 0),
                                                       c[1] + (axis == 1 ? sign : 0),
                                                       c[2] + (axis == 2 ? sign : 0));
                                        if (tab.cull_same[cur]) face_light = tab.light_coarse[face_light];
                                        const uint32_t ao = face_ao(c, axis, sign, ua, va);
                                        // В маске храним id/meta как представителя для UV-резолва;
                                        // соседние клетки с той же UV-областью могут слиться независимо от id.
                                        key = static_cast<uint64_t>(cur) |
                                              (static_cast<uint64_t>(meta) << 16) |
                                              (static_cast<uint64_t>(face_light) << 24) |
                                              (static_cast<uint64_t>(ao) << 32);
                                        any = true;
                                    }
                                }
                                S.mask[static_cast<size_t>(v) * kN + u] = key;
                            }
                        }
                        if (!any) continue;

                        for (int v = 0; v < v_count; ++v) {
                            for (int u = 0; u < kN;) {
                                const uint64_t key = S.mask[static_cast<size_t>(v) * kN + u];
                                if (key == 0) { ++u; continue; }

                                const Block_Properties& bp = *props[key & 0xFFFFu];
                                const uint8_t meta = static_cast<uint8_t>((key >> 16) & 0xFFu);
                                bool rotate = false;
                                const glm::vec4& uv = bp.uses_meta
                                    ? bp.resolve_face_uv(face, meta, rotate)
                                    : bp.get_uv(axis == 1 ? sign : 0);

                                // Повёрнутая текстура (бревно на боку) не сливается: квад 1x1.
                                int w = 1, h = 1;
                                bool key_rotate = false;
                                const Block_Properties& key_props = *props[key & 0xFFFFu];
                                const glm::vec4& key_uv = key_props.uses_meta
                                    ? key_props.resolve_face_uv(face, meta, key_rotate)
                                    : key_props.get_uv(axis == 1 ? sign : 0);
                                const auto same_surface = [&](uint64_t candidate) {
                                    if (candidate == 0) return false;
                                    const Block_Properties& candidate_props = *props[candidate & 0xFFFFu];
                                    const uint8_t candidate_meta = static_cast<uint8_t>((candidate >> 16) & 0xFFu);
                                    bool candidate_rotate = false;
                                    const glm::vec4& candidate_uv = candidate_props.uses_meta
                                        ? candidate_props.resolve_face_uv(face, candidate_meta, candidate_rotate)
                                        : candidate_props.get_uv(axis == 1 ? sign : 0);
                                    return key_rotate == candidate_rotate &&
                                           key_uv.x == candidate_uv.x && key_uv.y == candidate_uv.y &&
                                           key_uv.z == candidate_uv.z && key_uv.w == candidate_uv.w;
                                };
                                if (!rotate) {
                                    while (u + w < kN &&
                                           same_surface(S.mask[static_cast<size_t>(v) * kN + u + w])) ++w;
                                    bool grow = true;
                                    while (v + h < v_count && grow) {
                                        for (int i = 0; i < w; ++i) {
                                            if (!same_surface(S.mask[static_cast<size_t>(v + h) * kN + u + i])) {
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
                                emit_terrain_quad(meshes.solid,
                                                  static_cast<float>(base_x + c[0]),
                                                  static_cast<float>(base_y + c[1]),
                                                  static_cast<float>(base_z + c[2]),
                                                  axis, sign, w, h, uv,
                                                  static_cast<float>((key >> 24) & 0xFFu), rotate,
                                                  (key >> 32) & 0xFFu);
                                u += w;
                            }
                        }
                    }
                }
            }
        }
        if (!all_air) {
            // --- Флора и жидкость: по одной клетке ---
            for (int y = y_begin; y < y_end; ++y) {
                if (layer_block_count[y] == 0) continue;
                for (int z = 0; z < kN; ++z) {
                    for (int x = 0; x < kN; ++x) {
                        const Block_ID id = S.blocks[(static_cast<size_t>(y) * kN + z) * kN + x];
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
                            const Block_ID nid = block_at(nx, ny, nz);

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
        range.has_geometry = (range.solid_count + range.flora_count + range.liquid_count) > 0 || section_greedy;

        if (range.has_geometry) {
            // GPU-секция: её квады лежат внутри Y-диапазона секции, поэтому он и есть границы.
            range.min_y = section_greedy ? static_cast<float>(base_y + y_begin) : std::numeric_limits<float>::max();
            range.max_y = section_greedy ? static_cast<float>(base_y + y_end) : std::numeric_limits<float>::lowest();
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

namespace {
    // Упаковка одного канала GPU-нагрузки: палитра (уникальные значения в порядке появления) и
    // номера фиксированной битности: 0 бит (одно значение) или 1..8 бит. Номер i лежит в битах
    // [i*bits, (i+1)*bits), младший бит первым, в uint32-словах; значение может пересекать границу
    // двух слов. Декодер в mesh_greedy.comp читает ровно так же. header: палитра, слова, битность.
    template <typename T>
    void pack_channel(const T* values, size_t count, std::vector<uint32_t>& out,
                      std::array<uint32_t, 3>& header) {
        // Таблица "значение -> номер в палитре канала": значения могут быть любой ширины (id блоков
        // до 16 бит). Заполняем только использованные записи и сбрасываем их обратно после прохода.
        thread_local std::vector<int> index;
        if (index.size() < (size_t(1) << (8 * sizeof(T)))) index.assign(size_t(1) << (8 * sizeof(T)), -1);
        std::vector<T> palette;
        for (size_t i = 0; i < count; ++i) {
            const T v = values[i];
            if (index[v] < 0) {
                index[v] = static_cast<int>(palette.size());
                palette.push_back(v);
            }
        }

        uint32_t bits = 0;
        if (palette.size() > 1) {
            bits = 1;
            while ((static_cast<size_t>(1) << bits) < palette.size()) ++bits;
        }

        header[0] = static_cast<uint32_t>(out.size());
        for (const T v : palette) out.push_back(static_cast<uint32_t>(v));
        header[1] = static_cast<uint32_t>(out.size());
        header[2] = bits;
        if (bits != 0) {
            const size_t base = out.size();
            out.resize(base + (count * bits + 31) / 32, 0u);
            for (size_t i = 0; i < count; ++i) {
                const uint32_t idx = static_cast<uint32_t>(index[values[i]]);
                const size_t bit = i * bits;
                const size_t w = base + (bit >> 5);
                const unsigned s = static_cast<unsigned>(bit & 31);
                out[w] |= idx << s;
                if (s + bits > 32) out[w + 1] |= idx >> (32 - s);
            }
        }
        for (const T v : palette) index[v] = -1;
    }
}

void build_gpu_solid_payload(Chunk_Meshes& meshes,
                             const Chunk_Block_Grid& blocks,
                             const Chunk_Light_Grid& light,
                             const Chunk_Block_Faces& neighbor_blocks,
                             const Chunk_Light_Faces& neighbor_light,
                             int chunk_x, int chunk_y, int chunk_z,
                             const Chunk_Neighbor_Lookup& neighbor_lookup,
                             const Chunk_Light_Lookup& light_lookup) {
    meshes.gpu_words.clear();
    meshes.gpu_channels = {};
    meshes.gpu_upload_pending = false;
    meshes.gpu_candidates = false;

    const bool all_air = blocks.is_uniform() && blocks.uniform_block() == Block_Types::Air;
    if (all_air) return;

    Mesh_Scratch& S = mesh_scratch();
    const Mesh_Tables& tab = mesh_tables();
    blocks.unpack(S.blocks.data());

    bool any_greedy = false;
    for (size_t i = 0; i < kVoxels && !any_greedy; ++i) any_greedy = tab.greedy[S.blocks[i]] != 0;
    if (!any_greedy) return;
    meshes.gpu_candidates = true;

    // Грань соседа нужна, только если на СВОЕЙ границе с этой стороны есть жадно-мешаемый блок.
    // Порядок граней как в Chunk_Manager: 0 -X, 1 +X, 2 -Y, 3 +Y, 4 -Z, 5 +Z.
    std::array<bool, 6> face_needed{};
    for (int face = 0; face < 6; ++face) {
        const int axis = face >> 1;
        const int coord = (face & 1) ? kN - 1 : 0;
        bool needed = false;
        for (int a = 0; a < kN && !needed; ++a) {
            for (int b = 0; b < kN && !needed; ++b) {
                int x = coord, y = b, z = a;
                if (axis == 1) { x = a; y = coord; z = b; }
                else if (axis == 2) { x = a; y = b; z = coord; }
                needed = tab.greedy[S.blocks[(static_cast<size_t>(y) * kN + z) * kN + x]] != 0;
            }
        }
        face_needed[face] = needed;
    }

    light.unpack(S.light.data());
    std::fill(S.meta.begin(), S.meta.end(), static_cast<uint8_t>(0));
    if (blocks.has_meta()) {
        for (const auto& [linear_index, value] : blocks.meta_entries()) {
            S.meta[linear_index] = value;
        }
    }

    // Грани соседей: блоки в S.border_block, свет в S.border_light, ячейка (a,b) -> b*32+a.
    const int base_x = chunk_x * kN;
    const int base_y = chunk_y * kH;
    const int base_z = chunk_z * kN;
    for (int face = 0; face < 6; ++face) {
        Block_ID* dst_blocks = S.border_block.data() + static_cast<size_t>(face) * kLayer;
        uint8_t* dst_light = S.border_light.data() + static_cast<size_t>(face) * kLayer;
        if (!face_needed[face]) {
            std::fill(dst_blocks, dst_blocks + kLayer, static_cast<Block_ID>(0));
            std::fill(dst_light, dst_light + kLayer, static_cast<uint8_t>(0));
            continue;
        }
        const bool have_blocks = neighbor_blocks[face].has_value();
        const bool have_light = neighbor_light[face].has_value();
        if (have_blocks) std::memcpy(dst_blocks, neighbor_blocks[face]->data.data(), kLayer);
        if (have_light) std::memcpy(dst_light, neighbor_light[face]->data.data(), kLayer);
        if (have_blocks && have_light) continue;

        // Соседнего чанка нет в снимке: значения берутся теми же лукапами, что и у CPU-мешера.
        for (int b = 0; b < kN; ++b) {
            for (int a = 0; a < kN; ++a) {
                int wx = base_x, wy = base_y, wz = base_z;
                switch (face) {
                    case 0: wx = base_x - 1;  wy = base_y + b; wz = base_z + a; break;
                    case 1: wx = base_x + kN; wy = base_y + b; wz = base_z + a; break;
                    case 2: wx = base_x + a;  wy = base_y - 1; wz = base_z + b; break;
                    case 3: wx = base_x + a;  wy = base_y + kH; wz = base_z + b; break;
                    case 4: wx = base_x + a;  wy = base_y + b; wz = base_z - 1; break;
                    default: wx = base_x + a; wy = base_y + b; wz = base_z + kN; break;
                }
                const size_t i = static_cast<size_t>(b) * kN + a;
                if (!have_blocks) {
                    dst_blocks[i] = neighbor_lookup
                        ? static_cast<Block_ID>((neighbor_lookup(wx, wy, wz)))
                        : static_cast<uint8_t>(0);
                }
                if (!have_light) {
                    dst_light[i] = light_lookup ? light_lookup(wx, wy, wz) : static_cast<uint8_t>(0);
                }
            }
        }
    }

    pack_channel(S.blocks.data(), kVoxels, meshes.gpu_words, meshes.gpu_channels[0]);
    pack_channel(S.meta.data(), kVoxels, meshes.gpu_words, meshes.gpu_channels[1]);
    pack_channel(S.light.data(), kVoxels, meshes.gpu_words, meshes.gpu_channels[2]);
    for (int face = 0; face < 6; ++face) {
        pack_channel(S.border_block.data() + static_cast<size_t>(face) * kLayer, kLayer,
                     meshes.gpu_words, meshes.gpu_channels[3 + face]);
        pack_channel(S.border_light.data() + static_cast<size_t>(face) * kLayer, kLayer,
                     meshes.gpu_words, meshes.gpu_channels[9 + face]);
    }
    // Кольцо рёбер и углов (канал 15): для AO у границы. Значения берёт тот же лукап соседей,
    // что и CPU-мешер, поэтому пустые соседи и область захвата обрабатываются одинаково.
    for (int idx = 0; idx < Chunk_Ring::kSize; ++idx) {
        int x = 0, y = 0, z = 0;
        chunk_ring_coords(idx, x, y, z);
        S.ring_block[idx] = neighbor_lookup
            ? static_cast<Block_ID>((neighbor_lookup(base_x + x, base_y + y, base_z + z)))
            : static_cast<uint8_t>(0);
    }
    pack_channel(S.ring_block.data(), Chunk_Ring::kSize, meshes.gpu_words, meshes.gpu_channels[15]);
    meshes.gpu_upload_pending = true;
}

Gpu_Mesh_Tables gpu_mesh_tables() {
    const Mesh_Tables& tab = mesh_tables();
    Gpu_Mesh_Tables out;
    const size_t capacity = Block_Registry::get_instance().id_capacity();
    out.flags.assign(capacity, 0u);
    for (size_t i = 0; i < capacity; ++i) {
        out.flags[i] = (tab.greedy[i] ? 1u : 0u) | (tab.transparent[i] ? 2u : 0u) |
                       (tab.uses_meta[i] ? 4u : 0u) | (tab.cull_same[i] ? 8u : 0u) |
                       (tab.occludes[i] ? 16u : 0u);
    }
    for (size_t i = 0; i < out.light_coarse.size(); ++i) {
        out.light_coarse[i] = tab.light_coarse[i];
    }
    return out;
}

bool block_occludes_view(Block_Types type) {
    const size_t id = static_cast<size_t>(static_cast<Block_ID>(type));
    return id < static_cast<size_t>(k_max_block_types) && mesh_tables().occludes[id] != 0;
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

    Chunk_Meshes meshes = build_chunk_mesh(m_blocks, m_block_light, m_chunk_x, m_chunk_y, m_chunk_z,
                                           lookup, light_lookup, *m_texture_atlas, false, meta_lookup);
    if (!same_connectivity(m_meshes, meshes)) g_connectivity_epoch.fetch_add(1, std::memory_order_relaxed);
    m_meshes = std::move(meshes);
    m_unmerged_solid.clear();
    m_unmerged_solid_counts.fill(0);
    m_is_mesh_dirty = false;
    m_mesh_version = ++g_mesh_version_counter;
}

void Chunk::apply_mesh(Chunk_Meshes meshes) {
    if (!same_connectivity(m_meshes, meshes)) g_connectivity_epoch.fetch_add(1, std::memory_order_relaxed);
    m_meshes = std::move(meshes);
    m_unmerged_solid.clear();
    m_unmerged_solid_counts.fill(0);
    m_is_mesh_dirty = false;
    m_mesh_version = ++g_mesh_version_counter;
}

bool Chunk::restore_unmerged_solid() {
    if (m_unmerged_solid.empty()) return false;
    if (!m_unmerged_solid.empty()) m_meshes.solid = m_unmerged_solid;
    m_meshes.extends_pos_x_blocks = 0;
    m_meshes.extends_pos_z_blocks = 0;
    if (!m_unmerged_solid.empty()) {
        size_t start = 0;
        for (size_t i = 0; i < m_meshes.sections.size(); ++i) {
            Chunk_Section_Range& section = m_meshes.sections[i];
            section.solid_start = start;
            section.solid_count = m_unmerged_solid_counts[i];
            start += section.solid_count;
            section.has_geometry = section.solid_count != 0 || section.flora_count != 0 || section.liquid_count != 0;
        }
    }
    return true;
}

void Chunk::cache_unmerged_solid(
    const std::array<size_t, Config::chunk_section_count>& section_counts) {
    if (!m_unmerged_solid.empty() || m_meshes.solid.empty()) return;
    m_unmerged_solid = m_meshes.solid;
    m_unmerged_solid_counts = section_counts;
}

void Chunk::touch_mesh_version() {
    m_mesh_version = ++g_mesh_version_counter;
}

namespace {
    constexpr float kQuad_Epsilon = 1.0e-4f;

    bool near_value(float a, float b) {
        return std::abs(a - b) <= kQuad_Epsilon;
    }

    struct Solid_Quad {
        std::array<Terrain_Vertex, 6> vertices{};
        std::array<float, 3> lo{};
        std::array<float, 3> hi{};
        // d(local_u, local_v) / d(world_x, world_y, world_z)
        std::array<std::array<float, 2>, 3> uv_slope{};
        int normal_axis = -1;
        int normal_sign = 0;
        bool flat_attributes = false;
    };

    Solid_Quad describe_solid_quad(const Terrain_Vertex* vertices) {
        Solid_Quad q;
        std::copy_n(vertices, 6, q.vertices.begin());
        q.lo = {vertices[0].x, vertices[0].y, vertices[0].z};
        q.hi = q.lo;
        for (int i = 1; i < 6; ++i) {
            const float p[3] = {vertices[i].x, vertices[i].y, vertices[i].z};
            for (int axis = 0; axis < 3; ++axis) {
                q.lo[axis] = std::min(q.lo[axis], p[axis]);
                q.hi[axis] = std::max(q.hi[axis], p[axis]);
            }
        }

        const glm::vec3 p0(vertices[0].x, vertices[0].y, vertices[0].z);
        const glm::vec3 p1(vertices[1].x, vertices[1].y, vertices[1].z);
        const glm::vec3 p2(vertices[2].x, vertices[2].y, vertices[2].z);
        const glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        const glm::vec3 an = glm::abs(n);
        if (an.x >= an.y && an.x >= an.z) q.normal_axis = 0;
        else if (an.y >= an.z) q.normal_axis = 1;
        else q.normal_axis = 2;
        const float normal_component = q.normal_axis == 0 ? n.x : q.normal_axis == 1 ? n.y : n.z;
        q.normal_sign = normal_component >= 0.0f ? 1 : -1;

        for (int axis = 0; axis < 3; ++axis) {
            const float extent = q.hi[axis] - q.lo[axis];
            if (extent <= kQuad_Epsilon) continue;
            float uv_lo[2] = {}, uv_hi[2] = {};
            int count_lo = 0, count_hi = 0;
            for (const Terrain_Vertex& v : q.vertices) {
                const float p = axis == 0 ? v.x : axis == 1 ? v.y : v.z;
                if (near_value(p, q.lo[axis])) {
                    uv_lo[0] += v.local_u; uv_lo[1] += v.local_v; ++count_lo;
                }
                if (near_value(p, q.hi[axis])) {
                    uv_hi[0] += v.local_u; uv_hi[1] += v.local_v; ++count_hi;
                }
            }
            if (count_lo && count_hi) {
                q.uv_slope[axis][0] = (uv_hi[0] / count_hi - uv_lo[0] / count_lo) / extent;
                q.uv_slope[axis][1] = (uv_hi[1] / count_hi - uv_lo[1] / count_lo) / extent;
            }
        }

        q.flat_attributes = true;
        for (int i = 1; i < 6; ++i) {
            const Terrain_Vertex& a = vertices[0];
            const Terrain_Vertex& b = vertices[i];
            q.flat_attributes = q.flat_attributes &&
                near_value(a.rect_u_min, b.rect_u_min) && near_value(a.rect_v_min, b.rect_v_min) &&
                near_value(a.rect_u_max, b.rect_u_max) && near_value(a.rect_v_max, b.rect_v_max) &&
                near_value(a.light, b.light) && near_value(a.ao, b.ao) && near_value(a.ny, b.ny);
        }
        return q;
    }

    using Boundary_Quad_Key = std::array<float, 14>;

    Boundary_Quad_Key boundary_quad_key(const Solid_Quad& q, int seam_axis) {
        Boundary_Quad_Key key{};
        size_t out = 0;
        key[out++] = static_cast<float>(q.normal_axis);
        key[out++] = static_cast<float>(q.normal_sign);
        key[out++] = q.lo[q.normal_axis]; // the shared face plane
        for (int axis = 0; axis < 3; ++axis) {
            if (axis == seam_axis || axis == q.normal_axis) continue;
            key[out++] = q.lo[axis];
            key[out++] = q.hi[axis];
        }
        const Terrain_Vertex& v = q.vertices[0];
        key[out++] = v.rect_u_min;
        key[out++] = v.rect_v_min;
        key[out++] = v.rect_u_max;
        key[out++] = v.rect_v_max;
        key[out++] = v.light;
        key[out++] = v.ao;
        key[out++] = v.ny;
        return key;
    }

    void extend_quad_across_seam(Solid_Quad& a, const Solid_Quad& b, int seam_axis) {
        const float old_edge = a.hi[seam_axis];
        const float extension = b.hi[seam_axis] - b.lo[seam_axis];
        for (Terrain_Vertex& v : a.vertices) {
            float* p = seam_axis == 0 ? &v.x : seam_axis == 1 ? &v.y : &v.z;
            if (!near_value(*p, old_edge)) continue;
            *p += extension;
            v.local_u += a.uv_slope[seam_axis][0] * extension;
            v.local_v += a.uv_slope[seam_axis][1] * extension;
        }
        a.hi[seam_axis] += extension;
    }

    void append_section_quads(const Chunk_Meshes& mesh, size_t section_index,
                              std::vector<std::array<Terrain_Vertex, 6>>& out) {
        const Chunk_Section_Range& section = mesh.sections[section_index];
        const size_t end = section.solid_start + section.solid_count;
        for (size_t i = section.solid_start; i + 6 <= end; i += 6) {
            std::array<Terrain_Vertex, 6> quad{};
            std::copy_n(mesh.solid.data() + i, 6, quad.begin());
            out.push_back(quad);
        }
    }

    void append_quads(std::vector<Terrain_Vertex>& target,
                      const std::vector<std::array<Terrain_Vertex, 6>>& quads) {
        target.reserve(target.size() + quads.size() * 6);
        for (const auto& quad : quads) target.insert(target.end(), quad.begin(), quad.end());
    }
}

bool merge_chunk_boundary_solids(Chunk& lower, Chunk& upper, int axis) {
    if ((axis != 0 && axis != 2) || lower.get_chunk_y() != upper.get_chunk_y()) return false;
    if (axis == 0 && (upper.get_chunk_x() <= lower.get_chunk_x() ||
                      upper.get_chunk_z() != lower.get_chunk_z())) return false;
    if (axis == 2 && (upper.get_chunk_z() <= lower.get_chunk_z() ||
                      upper.get_chunk_x() != lower.get_chunk_x())) return false;

    Chunk_Meshes& a_mesh = lower.get_meshes_mutable();
    Chunk_Meshes& b_mesh = upper.get_meshes_mutable();
    if (a_mesh.solid_on_gpu || b_mesh.solid_on_gpu || a_mesh.solid.empty() || b_mesh.solid.empty()) return false;

    // Слияние работает только с квадами, упирающимися в шов (нижний кончается на нём, верхний на нём
    // начинается). Раньше для каждого вызова копировались ВСЕ квады обоих чанков по секциям и строился
    // std::map, хотя по замерам 99 % вызовов ничего не сливали, а остальные трогали десяток квадов.
    // Теперь: один линейный проход по вершинам находит кандидатов, ключ считается только для них,
    // а меш переписывается, только если пара реально нашлась (и тогда b уплотняется за один проход).
    const float boundary = static_cast<float>(
        (axis == 0 ? upper.get_chunk_x() : upper.get_chunk_z()) * Config::chunk_size);
    const auto coord = [axis](const Terrain_Vertex& v) { return axis == 0 ? v.x : v.z; };

    struct Candidate {
        Boundary_Quad_Key key;
        size_t section;
        size_t offset;   // индекс первой вершины квада в mesh.solid
    };
    const auto collect = [&](const Chunk_Meshes& mesh, bool lower_side, std::vector<Candidate>& out) {
        for (size_t section_index = 0; section_index < mesh.sections.size(); ++section_index) {
            const Chunk_Section_Range& section = mesh.sections[section_index];
            const size_t end = section.solid_start + section.solid_count;
            for (size_t i = section.solid_start; i + 6 <= end; i += 6) {
                float lo = coord(mesh.solid[i]), hi = lo;
                for (size_t k = 1; k < 6; ++k) {
                    const float c = coord(mesh.solid[i + k]);
                    lo = std::min(lo, c);
                    hi = std::max(hi, c);
                }
                const bool touches = lower_side ? (near_value(hi, boundary) && lo < boundary - kQuad_Epsilon)
                                                : (near_value(lo, boundary) && hi > boundary + kQuad_Epsilon);
                if (!touches) continue;
                const Solid_Quad q = describe_solid_quad(mesh.solid.data() + i);
                if (!q.flat_attributes || q.normal_axis == axis) continue;
                out.push_back({boundary_quad_key(q, axis), section_index, i});
            }
        }
    };

    std::vector<Candidate> a_cands, b_cands;
    collect(a_mesh, true, a_cands);
    if (a_cands.empty()) return false;
    collect(b_mesh, false, b_cands);
    if (b_cands.empty()) return false;

    // Поиск пары: та же секция и тот же ключ; из нескольких равных берём первый по порядку в меше.
    const auto less = [](const Candidate& l, const Candidate& r) {
        if (l.section != r.section) return l.section < r.section;
        if (l.key != r.key) return l.key < r.key;
        return l.offset < r.offset;
    };
    std::sort(b_cands.begin(), b_cands.end(), less);
    std::vector<bool> b_removed(b_cands.size(), false);

    struct Merge_Pair { size_t a_offset; std::array<Terrain_Vertex, 6> extended; };
    std::vector<Merge_Pair> pairs;
    for (const Candidate& ac : a_cands) {
        Candidate probe{ac.key, ac.section, 0};
        auto it = std::lower_bound(b_cands.begin(), b_cands.end(), probe, less);
        Solid_Quad a = describe_solid_quad(a_mesh.solid.data() + ac.offset);
        for (; it != b_cands.end() && it->section == ac.section && it->key == ac.key; ++it) {
            const size_t bi = static_cast<size_t>(it - b_cands.begin());
            if (b_removed[bi]) continue;
            const Solid_Quad b = describe_solid_quad(b_mesh.solid.data() + it->offset);
            if (!b.flat_attributes || !near_value(b.lo[axis], boundary) || b.hi[axis] <= boundary) continue;

            bool same_uv_direction = true;
            for (int uv_axis = 0; uv_axis < 3; ++uv_axis) {
                same_uv_direction = same_uv_direction &&
                    near_value(a.uv_slope[uv_axis][0], b.uv_slope[uv_axis][0]) &&
                    near_value(a.uv_slope[uv_axis][1], b.uv_slope[uv_axis][1]);
            }
            if (!same_uv_direction) continue;

            extend_quad_across_seam(a, b, axis);
            pairs.push_back({ac.offset, a.vertices});
            b_removed[bi] = true;
            break;
        }
    }
    if (pairs.empty()) return false;

    // Оригиналы для restore_unmerged_solid: снимаются до правки (cache_unmerged_solid копирует solid как есть).
    std::array<size_t, Config::chunk_section_count> a_original_counts{};
    std::array<size_t, Config::chunk_section_count> b_original_counts{};
    for (size_t i = 0; i < a_mesh.sections.size(); ++i) {
        a_original_counts[i] = a_mesh.sections[i].solid_count;
        b_original_counts[i] = b_mesh.sections[i].solid_count;
    }
    lower.cache_unmerged_solid(a_original_counts);
    upper.cache_unmerged_solid(b_original_counts);

    // Нижний меш: квады растягиваются на месте, число вершин и диапазоны секций не меняются.
    for (const Merge_Pair& pair : pairs) {
        std::copy(pair.extended.begin(), pair.extended.end(), a_mesh.solid.begin() + pair.a_offset);
    }

    // Верхний меш: выбросить поглощённые квады, секции пересчитать.
    std::vector<size_t> removed_offsets;
    removed_offsets.reserve(pairs.size());
    for (size_t bi = 0; bi < b_cands.size(); ++bi) {
        if (b_removed[bi]) removed_offsets.push_back(b_cands[bi].offset);
    }
    std::sort(removed_offsets.begin(), removed_offsets.end());
    std::vector<Terrain_Vertex> new_b;
    new_b.reserve(b_mesh.solid.size());
    for (size_t section_index = 0; section_index < b_mesh.sections.size(); ++section_index) {
        Chunk_Section_Range& section = b_mesh.sections[section_index];
        const size_t start = section.solid_start;
        const size_t end = start + section.solid_count;
        const size_t new_start = new_b.size();
        for (size_t i = start; i + 6 <= end; i += 6) {
            if (std::binary_search(removed_offsets.begin(), removed_offsets.end(), i)) continue;
            new_b.insert(new_b.end(), b_mesh.solid.begin() + i, b_mesh.solid.begin() + i + 6);
        }
        section.solid_start = new_start;
        section.solid_count = new_b.size() - new_start;
    }
    b_mesh.solid = std::move(new_b);

    for (Chunk_Section_Range& section : a_mesh.sections) {
        section.has_geometry = section.solid_count != 0 || section.flora_count != 0 || section.liquid_count != 0;
    }
    for (Chunk_Section_Range& section : b_mesh.sections) {
        section.has_geometry = section.solid_count != 0 || section.flora_count != 0 || section.liquid_count != 0;
    }
    if (axis == 0) {
        a_mesh.extends_pos_x_blocks = std::max(
            a_mesh.extends_pos_x_blocks,
            (upper.get_chunk_x() - lower.get_chunk_x()) * Config::chunk_size);
    } else {
        a_mesh.extends_pos_z_blocks = std::max(
            a_mesh.extends_pos_z_blocks,
            (upper.get_chunk_z() - lower.get_chunk_z()) * Config::chunk_size);
    }
    lower.touch_mesh_version();
    upper.touch_mesh_version();
    return true;
}

bool Chunk::is_empty() const {
    return !m_meshes.gpu_candidates && m_meshes.solid.empty() &&
           m_meshes.flora.empty() && m_meshes.liquid.empty();
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
    mark_mesh_dirty();
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
    mark_mesh_dirty();
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
