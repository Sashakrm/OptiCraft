//
// Created by noktemor on 11.02.2026.
//

#include "world/chunks/Chunk.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Types.h"
#include "world/blocks/Block_Shapes.h"
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
        // Сохраняем capacity: shrink_to_fit на каждом пустом чанке приводит к
        // повторным освобождениям/выделениям при стриминге мира.
        m_words.clear();
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

    if (m_world_generator->chunk_is_definitely_air(m_chunk_x, m_chunk_y, m_chunk_z)) {
        finish_generation(true);
        return;
    }

    // Генерация базового террейна. При пакетной загрузке этот вызов выполняется
    // для нескольких вертикальных чанков сразу через World_Generator::generate_chunk_column.
    m_world_generator->generate_chunk(m_blocks, m_chunk_x, m_chunk_y, m_chunk_z);
    finish_generation(false);
}

void Chunk::finish_generation(bool already_known_air, bool layer_metadata_precomputed) {
    if (!m_world_generator) return;

    if (already_known_air) {
        m_blocks.fill(Block_Types::Air);
        m_layer_block_count.fill(0);
        m_is_modified = false;
        return;
    }

    const int start_x = m_chunk_x * Config::chunk_size;
    const int start_z = m_chunk_z * Config::chunk_size;

    // Декорации размещаются только после базового рельефа всех чанков колонки. Это важно
    // для деревьев/структур на границе Y: порядок готовности отдельных чанков не меняет результат.
    if (m_world_generator->may_decorate_chunk(m_chunk_x, m_chunk_y, m_chunk_z)) {
        for (int x = -Config::decoration_overhang;
             x < Config::chunk_size + Config::decoration_overhang; ++x) {
            for (int z = -Config::decoration_overhang;
                 z < Config::chunk_size + Config::decoration_overhang; ++z) {
                m_world_generator->decorate_column(*this, start_x + x, start_z + z);
            }
        }
    }

    // Декорации меняют сетку через set() — выкидываем ставшие ненужными записи палитры.
    m_blocks.compact();
    if (!layer_metadata_precomputed) recalculate_metadata();
    m_is_modified = false;
}

// ---------------------------------------------------------------------------
//  Подготовка меша чанка для GPU
// ---------------------------------------------------------------------------
// Геометрию чанка (грани, AO, свет, высоты воды) строят компьют-шейдеры: mesh_greedy.comp — жадные
// квады непрозрачных кубов, mesh_misc.comp — блоки формы, флора и жидкость. Здесь, на CPU, остаётся
// только то, что не геометрия: связность секций для отсечения укрытий (compute_section_connectivity),
// маски видов геометрии по секциям и упаковка блоков, метаданных и света в палитровые каналы.
namespace {
    constexpr int kN = Config::chunk_size;
    constexpr int kH = Config::chunk_height;
    static_assert(kN == kH, "mesher assumes cubic chunks");
    constexpr size_t kLayer = static_cast<size_t>(kN) * kN;
    constexpr size_t kVoxels = kLayer * kH;

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

    // Биты флагов блока для GPU (читают mesh_greedy.comp и mesh_misc.comp; менять вместе с ними).
    constexpr uint32_t kFlagGreedy = 1u, kFlagTransparent = 2u, kFlagUsesMeta = 4u, kFlagCullSame = 8u,
                       kFlagOccludes = 16u, kFlagFlora = 32u, kFlagLiquid = 64u, kFlagShaped = 128u,
                       kFlagShapeCulls = 256u, kFlagLiquidState = 512u;
    constexpr uint32_t kFlagShapeShift = 10u;
    // Виды геометрии (биты маски вида секции и чанка): 0 — непрозрачные кубы, 1 — блоки формы, 2 — флора, 3 — жидкость.
    constexpr uint8_t kKindSolid = 1, kKindShaped = 2, kKindFlora = 4, kKindLiquid = 8;

    // Плоские таблицы "id -> нужный бит" (целиком в L1): цикл по клеткам проверяет их, а не Block_Properties.
    struct Mesh_Tables {
        std::vector<uint8_t> greedy = std::vector<uint8_t>(k_max_block_types, 0);      // твёрдый блок, сливается жадным мешером
        std::vector<uint8_t> transparent = std::vector<uint8_t>(k_max_block_types, 0); // грань к такому соседу видна
        std::vector<uint8_t> uses_meta = std::vector<uint8_t>(k_max_block_types, 0);
        // Полупрозрачный кубический блок (листва, стекло, лёд, слизь): грань к такому же блоку
        // не рисуется (внутри кроны/глыбы граней нет).
        std::vector<uint8_t> cull_same = std::vector<uint8_t>(k_max_block_types, 0);
        // Непрозрачный полный куб (жадно-мешаемый и не прозрачный): закрывает AO соседних углов и обзор.
        std::vector<uint8_t> occludes = std::vector<uint8_t>(k_max_block_types, 0);
        std::vector<uint8_t> kinds = std::vector<uint8_t>(k_max_block_types, 0);       // kKind* блока
        std::vector<uint32_t> flags = std::vector<uint32_t>(k_max_block_types, 0);     // kFlag* для GPU
        // Упакованный свет (sky<<4 | block): точные значения, без огрубления.
        std::array<uint8_t, 256> light_coarse{};
    };
    const Mesh_Tables& mesh_tables() {
        static const Mesh_Tables tables = [] {
            Mesh_Tables m;
            for (size_t i = 0; i < k_max_block_types; ++i) {
                const Block_Properties& p = *props_table()[i];
                const bool shaped = p.shape != Block_Shape::None;
                const bool flora = !shaped && p.mesh_style == Block_Mesh_Style::XStyle;
                const bool liquid_state = p.state == Block_State::Liquid;
                const bool liquid = !shaped && !flora && liquid_state;
                m.greedy[i] = (i != 0 && !liquid_state && !flora && !shaped) ? 1 : 0;
                m.transparent[i] = p.is_transparent ? 1 : 0;
                m.uses_meta[i] = p.uses_meta ? 1 : 0;
                m.cull_same[i] = (m.greedy[i] && p.is_transparent) ? 1 : 0;
                m.occludes[i] = (m.greedy[i] && !p.is_transparent) ? 1 : 0;
                if (i != 0) {
                    m.kinds[i] = static_cast<uint8_t>((m.greedy[i] ? kKindSolid : 0) | (shaped ? kKindShaped : 0) |
                                                      (flora ? kKindFlora : 0) | (liquid ? kKindLiquid : 0));
                }
                m.flags[i] = (m.greedy[i] ? kFlagGreedy : 0u) | (p.is_transparent ? kFlagTransparent : 0u) |
                             (p.uses_meta ? kFlagUsesMeta : 0u) | (m.cull_same[i] ? kFlagCullSame : 0u) |
                             (m.occludes[i] ? kFlagOccludes : 0u) | (flora ? kFlagFlora : 0u) |
                             (liquid ? kFlagLiquid : 0u) | (shaped ? kFlagShaped : 0u) |
                             ((i != 0 && !shaped && !liquid_state && !p.is_transparent) ? kFlagShapeCulls : 0u) |
                             (liquid_state ? kFlagLiquidState : 0u) |
                             (static_cast<uint32_t>(p.shape) << kFlagShapeShift);
            }
            for (int packed = 0; packed < 256; ++packed) {
                m.light_coarse[static_cast<size_t>(packed)] = static_cast<uint8_t>(packed);
            }
            return m;
        }();
        return tables;
    }

    // Рабочие буферы подготовки — свои у каждого потока (мешинг идёт в воркерах), чтобы не
    // аллоцировать ~100 КБ на каждую задачу.
    struct Mesh_Scratch {
        std::array<Block_ID, kVoxels> blocks;      // id блоков чанка, плоско (y, z, x)
        std::array<Block_ID, kVoxels> lod_blocks;  // coarse-копия только для greedy-мешера
        std::array<uint8_t, kVoxels> lod_meta;   // метаданные выбранного представителя coarse-группы
        std::array<uint8_t, kVoxels> light;        // упакованный свет, плоско
        std::array<uint8_t, kVoxels> meta;         // метаданные блоков, плоско
        // Грани соседей за границей чанка: блоки, метаданные и свет, ячейка (a,b) -> b*32+a.
        std::array<Block_ID, 6 * kLayer> border_block;
        std::array<uint8_t, 6 * kLayer> border_meta;
        std::array<uint8_t, 6 * kLayer> border_light;
        // Кольцо рёбер и углов за границей (для AO и высот жидкости).
        std::array<Block_ID, Chunk_Ring::kSize> ring_block;
        std::array<uint8_t, Chunk_Ring::kSize> ring_meta;
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
    // Блоки секции лежат в S.blocks с y_begin, порядок — как в build_chunk_gpu_mesh (y, z, x).
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
}

namespace {
    // Упаковка одного канала GPU-нагрузки: палитра (уникальные значения в порядке появления) и
    // номера фиксированной битности: 0 бит (одно значение) или 1..16 бит. Номер i лежит в битах
    // [i*bits, (i+1)*bits), младший бит первым, в uint32-словах; значение может пересекать границу
    // двух слов. Декодеры в mesh_greedy.comp и mesh_misc.comp читают ровно так же. header: палитра, слова, битность.
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

Chunk_Meshes build_chunk_gpu_mesh(const Chunk_Block_Grid& blocks,
                                  const Chunk_Light_Grid& light,
                                  const Chunk_Block_Faces& neighbor_blocks,
                                  const Chunk_Light_Faces& neighbor_light,
                                  int chunk_x, int chunk_y, int chunk_z,
                                  const Chunk_Neighbor_Lookup& neighbor_lookup,
                                  const Chunk_Light_Lookup& light_lookup,
                                  const Chunk_Meta_Lookup& meta_lookup,
                                  bool only_positive_faces,
                                  int lod_stride) {
    Chunk_Meshes meshes;
    meshes.positive_faces_only = only_positive_faces;
    lod_stride = (lod_stride >= 4) ? 4 : (lod_stride >= 2 ? 2 : 1);
    meshes.lod_stride = static_cast<uint8_t>(lod_stride);

    const int base_x = chunk_x * kN;
    const int base_y = chunk_y * kH;
    const int base_z = chunk_z * kN;
    for (int s = 0; s < Config::chunk_section_count; ++s) {
        Chunk_Section_Range& range = meshes.sections[s];
        range.min_y = static_cast<float>(base_y + s * Config::chunk_section_height);
        range.max_y = range.min_y;
    }

    // Чанк целиком из воздуха ничего не рисует (грани на границе рисуют соседи).
    const bool all_air = blocks.is_uniform() && blocks.uniform_block() == Block_Types::Air;
    if (all_air) return meshes;

    Mesh_Scratch& S = mesh_scratch();
    const Mesh_Tables& tab = mesh_tables();
    blocks.unpack(S.blocks.data());


    // Виды геометрии по секциям и связность для отсечения укрытий.
    std::array<uint8_t, Config::chunk_section_count> section_kinds{};
    for (size_t i = 0; i < kVoxels; ++i) {
        section_kinds[i / (kLayer * Config::chunk_section_height)] |= tab.kinds[S.blocks[i]];
    }
    for (int s = 0; s < Config::chunk_section_count; ++s) {
        Chunk_Section_Range& range = meshes.sections[s];
        const int y_begin = s * Config::chunk_section_height;
        range.conn = compute_section_connectivity(S, y_begin);
        range.kinds = section_kinds[s];
        range.has_geometry = range.kinds != 0;
        if (range.has_geometry) {
            // Квады лежат внутри Y-диапазона секции (волна воды и покачивание флоры — доли блока).
            range.min_y = static_cast<float>(base_y + y_begin);
            range.max_y = static_cast<float>(base_y + y_begin + Config::chunk_section_height);
        }
        meshes.gpu_kinds = static_cast<uint8_t>(meshes.gpu_kinds | range.kinds);
    }
    if (meshes.gpu_kinds == 0) return meshes;

    // Грань соседа нужна, только если на СВОЕЙ границе с этой стороны есть блок, которому нужны соседи
    // (кубы с AO и светом, блоки формы, жидкость). Порядок граней как в Chunk_Manager: 0 -X, 1 +X, 2 -Y, 3 +Y, 4 -Z, 5 +Z.
    constexpr uint8_t kNeedsNeighbors = kKindSolid | kKindShaped | kKindLiquid;
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
                needed = (tab.kinds[S.blocks[(static_cast<size_t>(y) * kN + z) * kN + x]] & kNeedsNeighbors) != 0;
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

    // Дальние solid-чанки получают coarse-воксельную копию. Внешняя полоса шириной
    // lod_stride остаётся точной: границы соседних чанков поэтому совпадают даже при
    // разных LOD. Для каждой группы выбираем самый частый greedy-блок вместе с его
    // метаданными (ориентация/активное состояние); сложная геометрия по-прежнему
    // строится mesh_misc.comp из полноразмерного канала 0.
    if (lod_stride > 1 && (meshes.gpu_kinds & kKindSolid) != 0u) {
        S.lod_blocks.fill(static_cast<Block_ID>(Block_Types::Air));
        S.lod_meta.fill(0);
        // Сохраняем исходные блоки и метаданные по всему периметру чанка.
        for (int y = 0; y < kH; ++y) {
            for (int z = 0; z < kN; ++z) {
                for (int x = 0; x < kN; ++x) {
                    if (x < lod_stride || x >= kN - lod_stride ||
                        y < lod_stride || y >= kH - lod_stride ||
                        z < lod_stride || z >= kN - lod_stride) {
                        const size_t index = (static_cast<size_t>(y) * kN + z) * kN + x;
                        S.lod_blocks[index] = S.blocks[index];
                        S.lod_meta[index] = S.meta[index];
                    }
                }
            }
        }

        std::array<Block_ID, 64> ids{};
        std::array<uint8_t, 64> metas{};
        std::array<uint8_t, 64> counts{};
        for (int y = lod_stride; y < kH - lod_stride; y += lod_stride) {
            for (int z = lod_stride; z < kN - lod_stride; z += lod_stride) {
                for (int x = lod_stride; x < kN - lod_stride; x += lod_stride) {
                    ids.fill(static_cast<Block_ID>(Block_Types::Air));
                    metas.fill(0);
                    counts.fill(0);
                    int unique_count = 0;
                    int greedy_count = 0;

                    for (int dy = 0; dy < lod_stride; ++dy) {
                        for (int dz = 0; dz < lod_stride; ++dz) {
                            for (int dx = 0; dx < lod_stride; ++dx) {
                                const size_t index = (static_cast<size_t>(y + dy) * kN + (z + dz)) * kN + (x + dx);
                                const Block_ID id = S.blocks[index];
                                if (id == static_cast<Block_ID>(Block_Types::Air) ||
                                    (tab.flags[id] & kFlagGreedy) == 0u) continue;

                                ++greedy_count;
                                const uint8_t meta = S.meta[index];
                                int candidate = 0;
                                while (candidate < unique_count &&
                                       (ids[candidate] != id || metas[candidate] != meta)) ++candidate;
                                if (candidate == unique_count) {
                                    ids[unique_count] = id;
                                    metas[unique_count] = meta;
                                    counts[unique_count] = 0;
                                    ++unique_count;
                                }
                                ++counts[candidate];
                            }
                        }
                    }

                    Block_ID representative = static_cast<Block_ID>(Block_Types::Air);
                    uint8_t representative_meta = 0;
                    // Небольшие одиночные блоки не должны разрастаться в целый LOD-куб.
                    // Четверти заполнения достаточно, чтобы сохранить тонкий рельефный слой.
                    const int minimum_occupancy = (lod_stride * lod_stride * lod_stride) / 4;
                    if (unique_count > 0 && greedy_count >= minimum_occupancy) {
                        int best = 0;
                        for (int i = 1; i < unique_count; ++i) {
                            if (counts[i] > counts[best]) best = i;
                        }
                        representative = ids[best];
                        representative_meta = metas[best];
                    }

                    for (int dy = 0; dy < lod_stride; ++dy) {
                        for (int dz = 0; dz < lod_stride; ++dz) {
                            for (int dx = 0; dx < lod_stride; ++dx) {
                                const size_t index = (static_cast<size_t>(y + dy) * kN + (z + dz)) * kN + (x + dx);
                                S.lod_blocks[index] = representative;
                                S.lod_meta[index] = representative_meta;
                            }
                        }
                    }
                }
            }
        }
    }

    // Грани соседей: блоки, метаданные и свет, ячейка (a,b) -> b*32+a.
    for (int face = 0; face < 6; ++face) {
        Block_ID* dst_blocks = S.border_block.data() + static_cast<size_t>(face) * kLayer;
        uint8_t* dst_meta = S.border_meta.data() + static_cast<size_t>(face) * kLayer;
        uint8_t* dst_light = S.border_light.data() + static_cast<size_t>(face) * kLayer;
        if (!face_needed[face]) {
            std::fill(dst_blocks, dst_blocks + kLayer, static_cast<Block_ID>(0));
            std::fill(dst_meta, dst_meta + kLayer, static_cast<uint8_t>(0));
            std::fill(dst_light, dst_light + kLayer, static_cast<uint8_t>(0));
            continue;
        }
        const bool have_blocks = neighbor_blocks[face].has_value();
        const bool have_light = neighbor_light[face].has_value();
        if (have_blocks) {
            std::memcpy(dst_blocks, neighbor_blocks[face]->data.data(), kLayer * sizeof(Block_ID));
            std::memcpy(dst_meta, neighbor_blocks[face]->meta.data(), kLayer);
        }
        if (have_light) std::memcpy(dst_light, neighbor_light[face]->data.data(), kLayer);
        if (have_blocks && have_light) continue;

        // Соседнего чанка нет в снимке: значения берутся лукапами (генератор / область захвата).
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
                        ? static_cast<Block_ID>(neighbor_lookup(wx, wy, wz))
                        : static_cast<Block_ID>(0);
                    dst_meta[i] = meta_lookup ? meta_lookup(wx, wy, wz) : static_cast<uint8_t>(0);
                }
                if (!have_light) {
                    dst_light[i] = light_lookup ? light_lookup(wx, wy, wz) : static_cast<uint8_t>(0);
                }
            }
        }
    }

    // Кольцо рёбер и углов (каналы 15 и 22). Значения берёт тот же лукап соседей, поэтому пустые
    // соседи и область захвата обрабатываются одинаково с гранями.
    for (int idx = 0; idx < Chunk_Ring::kSize; ++idx) {
        int x = 0, y = 0, z = 0;
        chunk_ring_coords(idx, x, y, z);
        S.ring_block[idx] = neighbor_lookup
            ? static_cast<Block_ID>(neighbor_lookup(base_x + x, base_y + y, base_z + z))
            : static_cast<Block_ID>(0);
        S.ring_meta[idx] = meta_lookup ? meta_lookup(base_x + x, base_y + y, base_z + z) : static_cast<uint8_t>(0);
    }

    meshes.gpu_words.clear();
    pack_channel(S.blocks.data(), kVoxels, meshes.gpu_words, meshes.gpu_channels[0]);
    pack_channel(S.meta.data(), kVoxels, meshes.gpu_words, meshes.gpu_channels[1]);
    pack_channel(S.light.data(), kVoxels, meshes.gpu_words, meshes.gpu_channels[2]);
    for (int face = 0; face < 6; ++face) {
        pack_channel(S.border_block.data() + static_cast<size_t>(face) * kLayer, kLayer,
                     meshes.gpu_words, meshes.gpu_channels[3 + face]);
        pack_channel(S.border_light.data() + static_cast<size_t>(face) * kLayer, kLayer,
                     meshes.gpu_words, meshes.gpu_channels[9 + face]);
        pack_channel(S.border_meta.data() + static_cast<size_t>(face) * kLayer, kLayer,
                     meshes.gpu_words, meshes.gpu_channels[16 + face]);
    }
    pack_channel(S.ring_block.data(), Chunk_Ring::kSize, meshes.gpu_words, meshes.gpu_channels[15]);
    pack_channel(S.ring_meta.data(), Chunk_Ring::kSize, meshes.gpu_words, meshes.gpu_channels[22]);
    if (lod_stride > 1 && (meshes.gpu_kinds & kKindSolid) != 0u) {
        pack_channel(S.lod_blocks.data(), kVoxels, meshes.gpu_words, meshes.gpu_channels[23]);
        pack_channel(S.lod_meta.data(), kVoxels, meshes.gpu_words, meshes.gpu_channels[24]);
    } else {
        const Block_ID air = static_cast<Block_ID>(Block_Types::Air);
        const uint8_t zero = 0;
        pack_channel(&air, 1, meshes.gpu_words, meshes.gpu_channels[23]);
        pack_channel(&zero, 1, meshes.gpu_words, meshes.gpu_channels[24]);
    }
    meshes.gpu_upload_pending = true;
    return meshes;
}

Gpu_Mesh_Tables gpu_mesh_tables() {
    const Mesh_Tables& tab = mesh_tables();
    Gpu_Mesh_Tables out;
    const size_t capacity = Block_Registry::get_instance().id_capacity();
    out.flags.assign(capacity, 0u);
    for (size_t i = 0; i < capacity && i < tab.flags.size(); ++i) out.flags[i] = tab.flags[i];
    for (size_t i = 0; i < out.light_coarse.size(); ++i) {
        out.light_coarse[i] = tab.light_coarse[i];
    }
    // Боксы блоков формы (единица — 1/64 клетки): индекс [форма * 256 + метаданные] -> (число, смещение).
    out.shape_index.assign(8 * 256 * 2, 0u);
    for (uint32_t shape = 1; shape <= 5; ++shape) {
        for (uint32_t meta = 0; meta < 256; ++meta) {
            Shape_Box boxes[2];
            const int count = shape_boxes(static_cast<Block_Shape>(shape), static_cast<uint8_t>(meta), boxes);
            const size_t at = (static_cast<size_t>(shape) * 256 + meta) * 2;
            out.shape_index[at] = static_cast<uint32_t>(count);
            out.shape_index[at + 1] = static_cast<uint32_t>(out.shape_boxes.size() / 2);
            for (int b = 0; b < count; ++b) {
                const auto q = [](float v) { return static_cast<uint32_t>(std::lround(v * 64.0f)); };
                const Shape_Box& box = boxes[b];
                out.shape_boxes.push_back(q(box.lo.x) | (q(box.lo.y) << 7) | (q(box.lo.z) << 14) | (q(box.hi.x) << 21));
                out.shape_boxes.push_back(q(box.hi.y) | (q(box.hi.z) << 7));
            }
        }
    }
    if (out.shape_boxes.empty()) out.shape_boxes.assign(2, 0u);   // пустой SSBO не создаём
    return out;
}

bool block_occludes_view(Block_Types type) {
    const size_t id = static_cast<size_t>(static_cast<Block_ID>(type));
    return id < static_cast<size_t>(k_max_block_types) && mesh_tables().occludes[id] != 0;
}

void Chunk::apply_mesh(Chunk_Meshes meshes) {
    if (!same_connectivity(m_meshes, meshes)) g_connectivity_epoch.fetch_add(1, std::memory_order_relaxed);
    m_meshes = std::move(meshes);
    m_is_mesh_dirty = false;
    m_mesh_version = ++g_mesh_version_counter;
}

void Chunk::touch_mesh_version() {
    m_mesh_version = ++g_mesh_version_counter;
}

bool Chunk::is_empty() const {
    return m_meshes.gpu_kinds == 0;
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

