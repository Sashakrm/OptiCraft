#include "world/crafting/Sapling_Manager.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/generation/Tree_Generator.h"
#include "math/Fast_Rng.h"
#include "utils/logging/Logger.h"
#include <algorithm>
#include <limits>
#include <sstream>

namespace {
    constexpr float k_ticks_per_second = 20.0f;
    constexpr float k_retry_ticks = 100.0f;   // 5 секунд до новой попытки, если место занято

    // Длительность роста в тиках (20 тиков = 1 секунда) для маленькой / средней / большой ели.
    constexpr float k_single_min = 600.0f,  k_single_max = 1200.0f;
    constexpr float k_medium_min = 800.0f,  k_medium_max = 1900.0f;
    constexpr float k_large_min  = 2500.0f, k_large_max  = 4200.0f;

    // Блоки, которые дерево может занять без разрушения: воздух, трава/цветы (не твёрдые),
    // листва других деревьев. Жидкость, камень, брёвна и т.п. — место занято.
    bool free_for_tree(Block_Types block) {
        switch (block) {
            case Block_Types::Air:
            case Block_Types::Oak_Leaf:
            case Block_Types::Birch_Leaves:
            case Block_Types::Spruce_Leaves:
            case Block_Types::Jungle_Leaves:
            case Block_Types::Acacia_Leaves:
            case Block_Types::Slimewood_Leaves:
                return true;
            default:
                break;
        }
        const Block_Properties& props = get_block_props(block);
        return !props.is_solid && props.state != Block_State::Liquid;
    }

    // Все чанки, которые покрывает коробка [lo, hi] в мировых координатах, загружены.
    bool box_loaded(const Chunk_Manager& chunk_manager, const glm::ivec3& lo, const glm::ivec3& hi) {
        const glm::ivec3 c_lo = chunk_manager.get_chunk_coords_for_world(lo.x, lo.y, lo.z);
        const glm::ivec3 c_hi = chunk_manager.get_chunk_coords_for_world(hi.x, hi.y, hi.z);
        for (int cy = c_lo.y; cy <= c_hi.y; ++cy)
            for (int cz = c_lo.z; cz <= c_hi.z; ++cz)
                for (int cx = c_lo.x; cx <= c_hi.x; ++cx)
                    if (!chunk_manager.is_chunk_loaded(cx, cy, cz)) return false;
        return true;
    }
}

namespace {
    Tree_Type tree_of(Block_Types sapling) {
        switch (sapling) {
            case Block_Types::Oak_Sapling:       return Tree_Type::Oak;
            case Block_Types::Birch_Sapling:     return Tree_Type::Birch;
            case Block_Types::Jungle_Sapling:    return Tree_Type::Jungle;
            case Block_Types::Acacia_Sapling:    return Tree_Type::Acacia;
            case Block_Types::Slimewood_Sapling: return Tree_Type::Slimewood;
            default:                             return Tree_Type::Spruce;
        }
    }
}

bool Sapling_Manager::is_sapling(Block_Types type) {
    switch (type) {
        case Block_Types::Spruce_Sapling:
        case Block_Types::Oak_Sapling:
        case Block_Types::Birch_Sapling:
        case Block_Types::Jungle_Sapling:
        case Block_Types::Acacia_Sapling:
        case Block_Types::Slimewood_Sapling:
            return true;
        default:
            return false;
    }
}

Sapling_Manager::Sapling_Manager() : m_rng(std::random_device{}()) {}

bool Sapling_Manager::can_grow_on(Block_Types ground) {
    return ground == Block_Types::Grass || ground == Block_Types::Dirt;
}

std::vector<glm::ivec3> Sapling_Manager::cells_of(const glm::ivec3& origin, int size) {
    std::vector<glm::ivec3> cells;
    cells.reserve(static_cast<size_t>(size * size));
    for (int dz = 0; dz < size; ++dz)
        for (int dx = 0; dx < size; ++dx)
            cells.emplace_back(origin.x + dx, origin.y, origin.z + dz);
    return cells;
}

bool Sapling_Manager::is_cell(const glm::ivec3& pos, const glm::ivec3& origin, int size) const {
    return pos.y == origin.y &&
           pos.x >= origin.x && pos.x < origin.x + size &&
           pos.z >= origin.z && pos.z < origin.z + size;
}

bool Sapling_Manager::group_loaded(const Chunk_Manager& chunk_manager, const glm::ivec3& origin, int size) const {
    const glm::ivec3 lo = origin;
    const glm::ivec3 hi(origin.x + size - 1, origin.y, origin.z + size - 1);
    return box_loaded(chunk_manager, lo, hi);
}

float Sapling_Manager::roll_total(int size, Block_Types species) {
    // Одиночные деревья других видов растут как маленькая ель.
    if (species != Block_Types::Spruce_Sapling) return std::uniform_real_distribution<float>(k_single_min, k_single_max)(m_rng);
    if (size >= 3) return std::uniform_real_distribution<float>(k_large_min, k_large_max)(m_rng);
    if (size == 2) return std::uniform_real_distribution<float>(k_medium_min, k_medium_max)(m_rng);
    return std::uniform_real_distribution<float>(k_single_min, k_single_max)(m_rng);
}

void Sapling_Manager::make_single(const glm::ivec3& pos, Block_Types species) {
    m_member_origin[pos] = pos;
    Group g;
    g.size = 1;
    g.species = species;
    g.progress = 0.0f;
    g.total = roll_total(1, species);
    g.retry = 0.0f;
    m_groups[pos] = g;
}

// Ищет квадрат 3x3, затем 2x2 с pos внутри, который целиком состоит из саженцев, чьи группы меньше
// квадрата (одиночные или 2x2 для 3x3). Такой квадрат становится новой группой с новым таймером.
// Группы, которые попали в квадрат, распускаются; их оставшиеся саженцы пробуют сложиться заново.
void Sapling_Manager::form_group_around(const glm::ivec3& pos) {
    auto own = m_member_origin.find(pos);
    if (own == m_member_origin.end() || m_groups.at(own->second).species != Block_Types::Spruce_Sapling) return;

    for (int size : {3, 2}) {
        for (int ox = pos.x - size + 1; ox <= pos.x; ++ox) {
            for (int oz = pos.z - size + 1; oz <= pos.z; ++oz) {
                const glm::ivec3 origin(ox, pos.y, oz);
                const std::vector<glm::ivec3> cells = cells_of(origin, size);

                bool ready = true;
                for (const glm::ivec3& c : cells) {
                    auto mi = m_member_origin.find(c);
                    if (mi == m_member_origin.end() || m_groups.at(mi->second).size >= size ||
                        m_groups.at(mi->second).species != Block_Types::Spruce_Sapling) {
                        ready = false;
                        break;
                    }
                }
                if (!ready) continue;

                // Распускаем меньшие группы, которые попали в квадрат, и запоминаем их остатки.
                std::vector<glm::ivec3> leftovers;
                for (const glm::ivec3& c : cells) {
                    auto mi = m_member_origin.find(c);
                    if (mi == m_member_origin.end()) continue;   // уже распущена вместе с соседом
                    const glm::ivec3 old_origin = mi->second;
                    if (old_origin == c && m_groups.at(old_origin).size == 1) continue;
                    const int old_size = m_groups.at(old_origin).size;
                    m_groups.erase(old_origin);
                    for (const glm::ivec3& oc : cells_of(old_origin, old_size)) {
                        m_member_origin.erase(oc);
                        if (std::find(cells.begin(), cells.end(), oc) == cells.end()) leftovers.push_back(oc);
                    }
                }
                for (const glm::ivec3& c : cells) {
                    m_groups.erase(c);            // одиночная группа имеет origin == позиция саженца
                    m_member_origin.erase(c);
                }

                for (const glm::ivec3& c : cells) m_member_origin[c] = origin;
                Group g;
                g.size = size;
                g.species = Block_Types::Spruce_Sapling;
                g.progress = 0.0f;
                g.total = roll_total(size, g.species);
                g.retry = 0.0f;
                m_groups[origin] = g;

                for (const glm::ivec3& lo : leftovers) make_single(lo, Block_Types::Spruce_Sapling);
                for (const glm::ivec3& lo : leftovers) {
                    auto mi = m_member_origin.find(lo);
                    if (mi != m_member_origin.end() && m_groups.at(mi->second).size == 1) form_group_around(lo);
                }
                return;
            }
        }
    }
}

void Sapling_Manager::release(const glm::ivec3& origin, const std::function<bool(const glm::ivec3&)>& alive) {
    auto git = m_groups.find(origin);
    const int size = git != m_groups.end() ? git->second.size : 1;
    const Block_Types species = git != m_groups.end() ? git->second.species : Block_Types::Spruce_Sapling;
    m_groups.erase(origin);

    const std::vector<glm::ivec3> cells = cells_of(origin, size);
    for (const glm::ivec3& c : cells) m_member_origin.erase(c);
    for (const glm::ivec3& c : cells) {
        if (alive(c)) make_single(c, species);
    }
    for (const glm::ivec3& c : cells) {
        if (!alive(c)) continue;
        auto mi = m_member_origin.find(c);
        if (mi != m_member_origin.end() && m_groups.at(mi->second).size == 1) {
            form_group_around(c);
        }
    }
}

void Sapling_Manager::plant(const glm::ivec3& pos, Block_Types sapling) {
    if (m_member_origin.count(pos)) return;
    make_single(pos, sapling);
    form_group_around(pos);
}

void Sapling_Manager::remove(const glm::ivec3& pos) {
    auto it = m_member_origin.find(pos);
    if (it == m_member_origin.end()) return;
    const glm::ivec3 origin = it->second;
    release(origin, [&pos](const glm::ivec3& c) { return c != pos; });
}

void Sapling_Manager::clear() {
    m_member_origin.clear();
    m_groups.clear();
}

bool Sapling_Manager::try_grow(const glm::ivec3& origin, int size, Chunk_Manager& chunk_manager,
                               std::vector<glm::ivec3>& out_changed) {
    // Блок земли под саженцами; дерево стоит на нём, а ствол начинается с уровня саженцев.
    const int base_y = origin.y - 1;
    const int cx = origin.x + (size - 1) / 2;
    const int cz = origin.z + (size - 1) / 2;

    // 1) План дерева без записи в мир.
    std::vector<std::pair<glm::ivec3, Block_Types>> plan;
    const Tree_Generator::Block_Setter collect = [&plan](int x, int y, int z, Block_Types type) {
        plan.emplace_back(glm::ivec3(x, y, z), type);
    };
    Fast_Rng rng(static_cast<uint64_t>(m_rng()));
    const Block_Types species = chunk_manager.get_block_world(origin.x, origin.y, origin.z);
    if (species != Block_Types::Spruce_Sapling) {
        Tree_Generator::place_tree(tree_of(species), origin.x, base_y, origin.z, collect, rng);
    } else {
        switch (size) {
            case 1:  Tree_Generator::place_spruce_tree(origin.x, base_y, origin.z, collect, rng); break;
            case 2:  Tree_Generator::place_spruce_medium(cx, base_y, cz, collect, rng); break;
            default: Tree_Generator::place_spruce_large(cx, base_y, cz, collect, rng); break;
        }
    }
    if (plan.empty()) return false;

    // 2) Все чанки коробки дерева должны быть загружены, иначе строить нельзя.
    glm::ivec3 lo(std::numeric_limits<int>::max());
    glm::ivec3 hi(std::numeric_limits<int>::min());
    for (const auto& [p, type] : plan) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    if (!box_loaded(chunk_manager, lo, hi)) return false;

    // 3) Место: каждый блок плана должен попасть в воздух, траву или листву. Клетки саженцев
    //    не в счёт: они освобождаются под ствол.
    for (const auto& [p, type] : plan) {
        const bool own_cell = is_cell(p, origin, size);
        if (own_cell) continue;
        if (!free_for_tree(chunk_manager.get_block_world(p.x, p.y, p.z))) return false;
    }

    // 4) Ставим: сначала очищаем клетки саженцев, потом пишем дерево.
    for (const glm::ivec3& c : cells_of(origin, size)) {
        if (chunk_manager.set_block_world(c.x, c.y, c.z, Block_Types::Air)) out_changed.push_back(c);
    }
    for (const auto& [p, type] : plan) {
        if (chunk_manager.set_block_world(p.x, p.y, p.z, type)) out_changed.push_back(p);
    }

    LOG_INFO("Sapling at " + std::to_string(origin.x) + " " + std::to_string(origin.y) + " " +
             std::to_string(origin.z) + " grew into a tree (" + std::to_string(plan.size()) + " blocks)");
    return true;
}

void Sapling_Manager::update(float delta_time, Chunk_Manager& chunk_manager, std::vector<glm::ivec3>& out_changed) {
    const float ticks = delta_time * k_ticks_per_second;

    // Копируем origin'ы: release() и try_grow() меняют m_groups.
    std::vector<glm::ivec3> origins;
    origins.reserve(m_groups.size());
    for (const auto& [origin, group] : m_groups) origins.push_back(origin);

    for (const glm::ivec3& origin : origins) {
        auto git = m_groups.find(origin);
        if (git == m_groups.end()) continue;   // распущена при пересборке соседей
        const int size = git->second.size;

        if (!group_loaded(chunk_manager, origin, size)) continue;   // вне загруженной зоны не растём

        // Саженцы на месте? Если хоть один сломан или заменён — группа распадается.
        const Block_Types species = git->second.species;
        bool intact = true;
        for (const glm::ivec3& c : cells_of(origin, size)) {
            if (chunk_manager.get_block_world(c.x, c.y, c.z) != species) { intact = false; break; }
        }
        if (!intact) {
            release(origin, [&chunk_manager](const glm::ivec3& c) {
                return is_sapling(chunk_manager.get_block_world(c.x, c.y, c.z));
            });
            continue;
        }

        // Земля под группой ещё на месте? Если нет — саженцы падают.
        if (!can_grow_on(chunk_manager.get_block_world(origin.x, origin.y - 1, origin.z))) {
            for (const glm::ivec3& c : cells_of(origin, size)) {
                if (chunk_manager.set_block_world(c.x, c.y, c.z, Block_Types::Air)) out_changed.push_back(c);
            }
            release(origin, [](const glm::ivec3&) { return false; });
            continue;
        }

        Group& g = git->second;
        if (g.retry > 0.0f) {
            g.retry -= ticks;
            continue;
        }
        g.progress += ticks;
        if (g.progress < g.total) continue;

        if (try_grow(origin, size, chunk_manager, out_changed)) {
            // Дерево выросло: группа закончилась, её саженцы стали деревом.
            m_groups.erase(origin);
            for (const glm::ivec3& c : cells_of(origin, size)) m_member_origin.erase(c);
        } else {
            // Места не хватает — пробуем позже, таймер сохраняем.
            m_groups[origin].retry = k_retry_ticks;
        }
    }
}

std::string Sapling_Manager::serialize() const {
    std::ostringstream out;
    for (const auto& [origin, g] : m_groups) {
        out << origin.x << ' ' << origin.y << ' ' << origin.z << ' ' << g.size << ' '
            << g.progress << ' ' << g.total << ' ' << static_cast<int>(g.species) << '\n';
    }
    return out.str();
}

void Sapling_Manager::deserialize(const std::string& text) {
    clear();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        glm::ivec3 origin;
        int size = 1;
        float progress = 0.0f, total = 0.0f;
        if (!(fields >> origin.x >> origin.y >> origin.z >> size >> progress >> total)) continue;
        int species = static_cast<int>(Block_Types::Spruce_Sapling);   // старые файлы — только ели
        fields >> species;
        if (size < 1 || size > 3) continue;
        Group g;
        g.size = size;
        g.species = is_sapling(static_cast<Block_Types>(species)) ? static_cast<Block_Types>(species)
                                                                  : Block_Types::Spruce_Sapling;
        if (g.species != Block_Types::Spruce_Sapling) g.size = 1;
        g.progress = std::max(0.0f, progress);
        g.total = std::max(1.0f, total);
        m_groups[origin] = g;
        for (const glm::ivec3& c : cells_of(origin, g.size)) m_member_origin[c] = origin;
    }
}
