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

namespace {
    std::atomic<uint64_t> g_mesh_version_counter{0};
}

Chunk::Chunk(int chunk_x, int chunk_y, int chunk_z, const World_Generator& generator)
    : m_chunk_x(chunk_x)
    , m_chunk_y(chunk_y)
    , m_chunk_z(chunk_z)
    , m_world_generator(&generator)
{
    for (int y = 0; y < Config::chunk_height; ++y) {
        for (int z = 0; z < Config::chunk_size; ++z) {
            for (int x = 0; x < Config::chunk_size; ++x) {
                m_blocks.set(x, y, z, Block_Types::Air);
            }
        }
    }
}

void Chunk::recalculate_metadata() {
    m_layer_block_count.fill(0);
    for (int y = 0; y < Config::chunk_height; ++y) {
        for (int z = 0; z < Config::chunk_size; ++z) {
            for (int x = 0; x < Config::chunk_size; ++x) {
                if (m_blocks.get(x, y, z) != Block_Types::Air) {
                    ++m_layer_block_count[y];
                }
            }
        }
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

    recalculate_metadata();

    // Декорации ставятся через set_block и взвели бы флаг правки, хотя чанк
    // ровно такой, каким его выдаёт генератор.
    m_is_modified = false;
}

namespace {
    // Ищет блок за пределами текущего чанка через neighbor_lookup (мировые координаты).
    // Ровно как раньше Chunk::get_neighbor_block, только не привязано к живому объекту.
    Block_Types get_neighbor_block_data(const Chunk_Block_Grid& blocks,
                                        int chunk_x, int chunk_y, int chunk_z,
                                        const Chunk_Neighbor_Lookup& neighbor_lookup,
                                        int x, int y, int z, int dx, int dy, int dz) {
        const int nx = x + dx;
        const int ny = y + dy;
        const int nz = z + dz;

        if (nx >= 0 && nx < Config::chunk_size &&
            ny >= 0 && ny < Config::chunk_height &&
            nz >= 0 && nz < Config::chunk_size) {
            return blocks.get(nx, ny, nz);
        }

        if (neighbor_lookup) {
            const int wx = chunk_x * Config::chunk_size + nx;
            const int wy = chunk_y * Config::chunk_height + ny;
            const int wz = chunk_z * Config::chunk_size + nz;
            return neighbor_lookup(wx, wy, wz);
        }

        return Block_Types::Air;
    }

    // Тот же принцип, что у get_neighbor_block_data, но для уровня света (см. Chunk_Light_Grid) —
    // используется, чтобы узнать, чем освещена ГРАНЬ блока (светом клетки, в которую эта грань
    // "смотрит"), а не самим твёрдым блоком (твёрдые блоки светом изнутри не светятся).
    uint8_t get_neighbor_light_data(const Chunk_Light_Grid& light,
                                    int chunk_x, int chunk_y, int chunk_z,
                                    const Chunk_Light_Lookup& light_lookup,
                                    int x, int y, int z, int dx, int dy, int dz) {
        const int nx = x + dx;
        const int ny = y + dy;
        const int nz = z + dz;

        if (nx >= 0 && nx < Config::chunk_size &&
            ny >= 0 && ny < Config::chunk_height &&
            nz >= 0 && nz < Config::chunk_size) {
            return light.get(nx, ny, nz);
        }

        if (light_lookup) {
            const int wx = chunk_x * Config::chunk_size + nx;
            const int wy = chunk_y * Config::chunk_height + ny;
            const int wz = chunk_z * Config::chunk_size + nz;
            return light_lookup(wx, wy, wz);
        }

        return 0;
    }

    bool is_face_visible_data(const Chunk_Block_Grid& blocks,
                              int chunk_x, int chunk_y, int chunk_z,
                              const Chunk_Neighbor_Lookup& neighbor_lookup,
                              int x, int y, int z, int dx, int dy, int dz) {
        const Block_Types neighbor = get_neighbor_block_data(blocks, chunk_x, chunk_y, chunk_z,
                                                              neighbor_lookup, x, y, z, dx, dy, dz);
        const Block_Properties& neighbor_props = get_block_props(neighbor);
        const Block_Properties& current_props = get_block_props(blocks.get(x, y, z));

        // Жидкость рисует грань ТОЛЬКО там, где с той стороны воздух.
        //
        // Общее правило "сосед прозрачный -> грань видна" для воды работало плохо:
        // вода прозрачна сама для себя, поэтому у каждого вокселя внутри водоёма
        // рисовались все шесть граней. Для водоёма глубиной 10 блоков взгляд
        // сверху пробивал десять слоёв полупрозрачного синего подряд — отсюда и
        // "сквозь воду рендерится вообще всё": это не один слой воды, а десятки
        // наложенных друг на друга квадов плюс вся геометрия за ними.
        //
        // Теперь у водоёма остаётся ровно его видимая оболочка со стороны воздуха:
        // поверхность и открытые боковые стенки у берега. Грани вода-вода,
        // вода-камень и вода-листва не попадают в меш вообще.
        if (current_props.state == Block_State::Liquid) {
            return neighbor == Block_Types::Air;
        }

        return neighbor_props.is_transparent;
    }

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

    void add_solid_face_data(Chunk_Meshes& meshes,
                             int chunk_x, int chunk_y, int chunk_z,
                             int x, int y, int z, int dx, int dy, int dz, Block_Types block,
                             float face_light, uint8_t meta = 0) {
        const float wx = static_cast<float>(chunk_x * Config::chunk_size + x);
        const float wy = static_cast<float>(chunk_y * Config::chunk_height + y);
        const float wz = static_cast<float>(chunk_z * Config::chunk_size + z);

        const Block_Properties& props = get_block_props(block);
        std::vector<Chunk_Vertex>& target = (props.state == Block_State::Liquid) ? meshes.liquid : meshes.solid;

        if (props.uses_meta) {
            // Ориентированный/двухсостоятельный блок: грань выбирается по метаданным.
            bool rotate = false;
            const glm::vec4& uv = props.resolve_face_uv(face_index(dx, dy, dz), meta, rotate);
            push_face(target, uv, wx, wy, wz, dx, dy, dz, face_light, rotate);
            return;
        }

        const int face_direction = dy;
        const glm::vec4& uv = props.get_uv(face_direction);
        push_face(target, uv, wx, wy, wz, dx, dy, dz, face_light);
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

Chunk_Meshes build_chunk_mesh(const Chunk_Block_Grid& blocks,
                              const Chunk_Light_Grid& light,
                              int chunk_x, int chunk_y, int chunk_z,
                              const Chunk_Neighbor_Lookup& neighbor_lookup,
                              const Chunk_Light_Lookup& light_lookup,
                              const Texture_Atlas& atlas,
                              bool only_positive_faces) {
    // UV блоков теперь резолвятся один раз при загрузке blocks.json (см.
    // Block_Registry::load_from_file → resolve_sprite_uv) и лежат готовыми
    // vec4 прямо в Block_Properties — так что здесь atlas для самой геометрии
    // больше не нужен. Параметр оставлен как "атлас обязан быть загружен к
    // этому моменту" сигнал вызывающей стороне (Chunk::build_mesh уже
    // проверяет его на nullptr выше по цепочке) — не трогаем сигнатуру, чтобы
    // не тащить более рискованную правку через Chunk_Manager/потоки мешинга.
    (void)atlas;
    Chunk_Meshes meshes;

    // Layer culling: пересчитывается из переданного снимка блоков (дёшево — 32×32 на слой).
    std::array<int, Config::chunk_height> layer_block_count{};
    for (int y = 0; y < Config::chunk_height; ++y) {
        int count = 0;
        for (int z = 0; z < Config::chunk_size; ++z) {
            for (int x = 0; x < Config::chunk_size; ++x) {
                if (blocks.get(x, y, z) != Block_Types::Air) ++count;
            }
        }
        layer_block_count[y] = count;
    }

    for (int section_index = 0; section_index < Config::chunk_section_count; ++section_index) {
        Chunk_Section_Range& range = meshes.sections[section_index];
        range = Chunk_Section_Range{};
        range.solid_start = meshes.solid.size();
        range.flora_start = meshes.flora.size();
        range.liquid_start = meshes.liquid.size();

        const int y_begin = section_index * Config::chunk_section_height;
        const int y_end = y_begin + Config::chunk_section_height;

        for (int y = y_begin; y < y_end; ++y) {
            if (layer_block_count[y] == 0) continue;

            for (int z = 0; z < Config::chunk_size; ++z) {
                for (int x = 0; x < Config::chunk_size; ++x) {
                    const Block_Types block = blocks.get(x, y, z);
                    if (block == Block_Types::Air) continue;

                    const Block_Properties& props = get_block_props(block);

                    if (props.mesh_style == Block_Mesh_Style::XStyle) {
                        const float own_light = static_cast<float>(light.get(x, y, z));
                        add_flora_block_data(meshes, chunk_x, chunk_y, chunk_z, x, y, z, block, own_light);
                        continue;
                    }

                    const auto visible = [&](int dx, int dy, int dz) {
                        return is_face_visible_data(blocks, chunk_x, chunk_y, chunk_z, neighbor_lookup, x, y, z, dx, dy, dz);
                    };
                    // Грань освещена клеткой, в которую она "смотрит" (соседняя прозрачная
                    // клетка) — твёрдый блок сам по себе не светится изнутри наружу.
                    const auto face_light = [&](int dx, int dy, int dz) {
                        return static_cast<float>(get_neighbor_light_data(light, chunk_x, chunk_y, chunk_z,
                                                                           light_lookup, x, y, z, dx, dy, dz));
                    };
                    const uint8_t meta = props.uses_meta ? blocks.get_meta(x, y, z) : 0;
                    if (visible(1, 0, 0)) add_solid_face_data(meshes, chunk_x, chunk_y, chunk_z, x, y, z, 1, 0, 0, block, face_light(1,0,0), meta);
                    if (!only_positive_faces && visible(-1, 0, 0)) add_solid_face_data(meshes, chunk_x, chunk_y, chunk_z, x, y, z, -1, 0, 0, block, face_light(-1,0,0), meta);
                    if (visible(0, 1, 0)) add_solid_face_data(meshes, chunk_x, chunk_y, chunk_z, x, y, z, 0, 1, 0, block, face_light(0,1,0), meta);
                    if (!only_positive_faces && visible(0, -1, 0)) add_solid_face_data(meshes, chunk_x, chunk_y, chunk_z, x, y, z, 0, -1, 0, block, face_light(0,-1,0), meta);
                    if (visible(0, 0, 1)) add_solid_face_data(meshes, chunk_x, chunk_y, chunk_z, x, y, z, 0, 0, 1, block, face_light(0,0,1), meta);
                    if (!only_positive_faces && visible(0, 0, -1)) add_solid_face_data(meshes, chunk_x, chunk_y, chunk_z, x, y, z, 0, 0, -1, block, face_light(0,0,-1), meta);
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
            const auto expand = [&range](const std::vector<Chunk_Vertex>& verts, size_t start, size_t count) {
                for (size_t i = start; i < start + count; ++i) {
                    range.min_y = std::min(range.min_y, verts[i].y);
                    range.max_y = std::max(range.max_y, verts[i].y);
                }
            };
            expand(meshes.solid, range.solid_start, range.solid_count);
            expand(meshes.flora, range.flora_start, range.flora_count);
            expand(meshes.liquid, range.liquid_start, range.liquid_count);
        } else {
            range.min_y = static_cast<float>(chunk_y * Config::chunk_height + y_begin);
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

    m_meshes = build_chunk_mesh(m_blocks, m_block_light, m_chunk_x, m_chunk_y, m_chunk_z,
                                lookup, light_lookup, *m_texture_atlas);
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
