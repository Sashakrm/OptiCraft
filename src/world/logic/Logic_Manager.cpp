#include "world/logic/Logic_Manager.h"
#include "world/chunks/Chunk_Manager.h"
#include "utils/Config.h"

#include <algorithm>

namespace {

// +X, -X, +Y, -Y, +Z, -Z — как грани в Block_Face.
const glm::ivec3 kDirs[6] = {
    {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
};

Redstone_Key key_of(glm::ivec3 p) { return {p.x, p.y, p.z}; }
glm::ivec3 pos_of(const Redstone_Key& k) { return {k[0], k[1], k[2]}; }

// Смещение выхода повторителя по направлению (Block_Meta::facing_*).
glm::ivec3 facing_offset(uint8_t meta) {
    switch (meta & Block_Meta::direction_mask) {
        case Block_Meta::facing_north: return {0, 0, -1};
        case Block_Meta::facing_east:  return {1, 0, 0};
        case Block_Meta::facing_south: return {0, 0, 1};
        default:                       return {-1, 0, 0};
    }
}

// Задержка повторителя в редстоун-тиках: биты 2..3 хранят 0..3 → 1..4.
int repeater_delay(uint8_t meta) { return ((meta >> 2) & 3) + 1; }

constexpr uint8_t kAttachMask = 0x07;     // грань крепления (индекс в kDirs)
constexpr uint8_t kDelayMask = 0x0C;      // биты задержки повторителя

// Сигнал, который узел n выдаёт в соседнюю клетку c.
int emit(Block_Types type, uint8_t meta, glm::ivec3 n, glm::ivec3 c) {
    switch (type) {
        case Block_Types::Lever_On:
        case Block_Types::Button_Pressed:
            return 15;
        case Block_Types::Signal_Clamp:
            return c == Logic_Manager::support_of(n, type, meta) ? 0 : 15;
        case Block_Types::Repeater_On:
            return c == n + facing_offset(meta) ? 15 : 0;
        default:
            return 0;
    }
}

// Чтение мира для одного шага симуляции. Непрогруженные клетки — воздух.
struct Sim {
    const Logic_Manager::World_Access& world;
    std::map<Redstone_Key, int> wire_power;

    Block_Types type(glm::ivec3 p) const {
        return world.loaded(p.x, p.y, p.z) ? world.type(p.x, p.y, p.z) : Block_Types::Air;
    }
    uint8_t meta(glm::ivec3 p) const { return world.meta(p.x, p.y, p.z); }

    // Сигнал, который твёрдый блок s получает от соседних узлов (без проводов — чтобы не было
    // циклов при расчёте проводов).
    int source_power(glm::ivec3 s) const {
        int best = 0;
        for (const glm::ivec3& d : kDirs) {
            const glm::ivec3 n = s + d;
            const Block_Types t = type(n);
            if (Logic_Manager::is_node(t) && !Logic_Manager::is_wire(t)) {
                best = std::max(best, emit(t, meta(n), n, s));
            }
        }
        return best;
    }

    // Весь сигнал, который приходит в клетку c от всех соседей (провода, узлы, твёрдые блоки).
    int full_power(glm::ivec3 c) const {
        int best = 0;
        for (const glm::ivec3& d : kDirs) {
            const glm::ivec3 n = c + d;
            const Block_Types t = type(n);
            if (Logic_Manager::is_wire(t)) {
                const auto it = wire_power.find(key_of(n));
                if (it != wire_power.end()) best = std::max(best, it->second);
            } else if (Logic_Manager::is_node(t)) {
                best = std::max(best, emit(t, meta(n), n, c));
            } else if (Logic_Manager::is_support(t)) {
                best = std::max(best, source_power(n));
            }
        }
        return best;
    }

    // Сигнал, который приходит в повторитель p со стороны входа back.
    int repeater_input(glm::ivec3 back, glm::ivec3 p) const {
        const Block_Types t = type(back);
        if (Logic_Manager::is_wire(t)) {
            const auto it = wire_power.find(key_of(back));
            return it == wire_power.end() ? 0 : it->second;
        }
        if (Logic_Manager::is_node(t)) return emit(t, meta(back), back, p);
        if (Logic_Manager::is_support(t)) return full_power(back);
        return 0;
    }
};

}  // namespace

// --- Классификация ----------------------------------------------------------------------------

bool Logic_Manager::is_wire(Block_Types type) {
    return type == Block_Types::Copper_Wire;
}

bool Logic_Manager::is_node(Block_Types type) {
    switch (type) {
        case Block_Types::Copper_Wire:
        case Block_Types::Lever:
        case Block_Types::Lever_On:
        case Block_Types::Button:
        case Block_Types::Button_Pressed:
        case Block_Types::Signal_Clamp:
        case Block_Types::Signal_Clamp_Off:
        case Block_Types::Repeater:
        case Block_Types::Repeater_On:
            return true;
        default:
            return false;
    }
}

bool Logic_Manager::is_attached(Block_Types type) {
    switch (type) {
        case Block_Types::Lever:
        case Block_Types::Lever_On:
        case Block_Types::Button:
        case Block_Types::Button_Pressed:
        case Block_Types::Signal_Clamp:
        case Block_Types::Signal_Clamp_Off:
            return true;
        default:
            return false;
    }
}

bool Logic_Manager::is_support(Block_Types type) {
    if (type == Block_Types::Air || is_node(type)) return false;
    return get_block_props(type).is_solid;
}

Block_Types Logic_Manager::item_of(Block_Types type) {
    switch (type) {
        case Block_Types::Lever_On:         return Block_Types::Lever;
        case Block_Types::Button_Pressed:   return Block_Types::Button;
        case Block_Types::Signal_Clamp_Off: return Block_Types::Signal_Clamp;
        case Block_Types::Repeater_On:      return Block_Types::Repeater;
        default:                            return type;
    }
}

glm::ivec3 Logic_Manager::support_of(glm::ivec3 pos, Block_Types type, uint8_t meta) {
    if (is_attached(type)) return pos + kDirs[meta & kAttachMask];
    return pos + glm::ivec3(0, -1, 0);   // провод и повторитель стоят на полу
}

// --- Установка --------------------------------------------------------------------------------

bool Logic_Manager::placement(Block_Types type, const World_Access& world, glm::ivec3 place,
                              glm::ivec3 clicked, uint8_t repeater_facing, uint8_t& out_meta) {
    if (is_wire(type) || type == Block_Types::Repeater) {
        if (!is_support(world.type(place.x, place.y - 1, place.z))) return false;
        out_meta = type == Block_Types::Repeater ? static_cast<uint8_t>(repeater_facing & Block_Meta::direction_mask)
                                                 : 0;
        return true;
    }
    if (is_attached(type)) {
        const glm::ivec3 diff = clicked - place;
        for (uint8_t i = 0; i < 6; ++i) {
            if (kDirs[i] == diff) {
                if (!is_support(world.type(clicked.x, clicked.y, clicked.z))) return false;
                out_meta = i;
                return true;
            }
        }
    }
    return false;
}

bool Logic_Manager::has_support_for(const World_Access& world, glm::ivec3 pos, Block_Types type, uint8_t meta) {
    const glm::ivec3 s = support_of(pos, type, meta);
    return is_support(world.type(s.x, s.y, s.z));
}

std::vector<glm::ivec3> Logic_Manager::nodes_without_support(const World_Access& world, glm::ivec3 pos) {
    std::vector<glm::ivec3> lost;
    for (const glm::ivec3& d : kDirs) {
        const glm::ivec3 n = pos + d;
        const Block_Types t = world.type(n.x, n.y, n.z);
        if (!is_node(t)) continue;
        if (!has_support_for(world, n, t, world.meta(n.x, n.y, n.z))) lost.push_back(n);
    }
    return lost;
}

// --- Действия игрока --------------------------------------------------------------------------

bool Logic_Manager::interact(const World_Access& world, glm::ivec3 pos) {
    const Block_Types type = world.type(pos.x, pos.y, pos.z);
    const uint8_t meta = world.meta(pos.x, pos.y, pos.z);
    switch (type) {
        case Block_Types::Lever:
            write_block(world, pos, Block_Types::Lever_On, meta);
            track(pos);
            return true;
        case Block_Types::Lever_On:
            write_block(world, pos, Block_Types::Lever, meta);
            track(pos);
            return true;
        case Block_Types::Button:
            write_block(world, pos, Block_Types::Button_Pressed, meta);
            m_pulses[key_of(pos)] = k_button_ticks;
            track(pos);
            return true;
        case Block_Types::Button_Pressed:
            return true;   // уже нажата — ничего не меняем
        case Block_Types::Repeater:
        case Block_Types::Repeater_On: {
            const int next = repeater_delay(meta) % 4 + 1;   // 1 → 2 → 3 → 4 → 1
            const uint8_t new_meta = static_cast<uint8_t>((meta & ~kDelayMask) | (((next - 1) & 3) << 2));
            write_block(world, pos, type, new_meta);
            track(pos);
            return true;
        }
        default:
            return false;
    }
}

// --- Симуляция --------------------------------------------------------------------------------

void Logic_Manager::track(glm::ivec3 pos) {
    m_nodes.insert(key_of(pos));
}

void Logic_Manager::discover(const World_Access& world, glm::ivec3 center, int radius) {
    for (int x = center.x - radius; x <= center.x + radius; ++x) {
        for (int y = center.y - radius; y <= center.y + radius; ++y) {
            for (int z = center.z - radius; z <= center.z + radius; ++z) {
                if (!world.loaded(x, y, z)) continue;
                const Block_Types t = world.type(x, y, z);
                if (!is_node(t)) continue;
                const Redstone_Key k{x, y, z};
                m_nodes.insert(k);
                // Кнопка, сохранённая нажатой: таймер заново, иначе она залипнет навсегда.
                if (t == Block_Types::Button_Pressed && m_pulses.find(k) == m_pulses.end()) {
                    m_pulses[k] = k_button_ticks;
                }
            }
        }
    }
}

void Logic_Manager::update(const World_Access& world, float dt) {
    m_accumulator += dt;
    int guard = 0;   // не больше 10 тиков за кадр, чтобы не уйти в спираль после паузы
    while (m_accumulator >= k_redstone_tick_seconds && guard++ < 10) {
        step(world);
        m_accumulator -= k_redstone_tick_seconds;
    }
    if (guard >= 10) m_accumulator = 0.0f;
}

void Logic_Manager::step(const World_Access& world) {
    Sim sim{world, {}};

    // 1. Забыть узлы, которых больше нет (сломаны), и их таймеры. Непрогруженные — не трогаем.
    for (auto it = m_nodes.begin(); it != m_nodes.end();) {
        const glm::ivec3 p = pos_of(*it);
        if (!world.loaded(p.x, p.y, p.z)) { ++it; continue; }
        if (!is_node(world.type(p.x, p.y, p.z))) {
            m_pending.erase(*it);
            m_pulses.erase(*it);
            it = m_nodes.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Сигнал проводов по состоянию на начало тика: от источников, затем на −1 по проводам.
    std::vector<glm::ivec3> wires;
    for (const Redstone_Key& k : m_nodes) {
        const glm::ivec3 p = pos_of(k);
        if (!world.loaded(p.x, p.y, p.z) || !is_wire(world.type(p.x, p.y, p.z))) continue;
        wires.push_back(p);
        int seed = 0;
        for (const glm::ivec3& d : kDirs) {
            const glm::ivec3 n = p + d;
            const Block_Types t = sim.type(n);
            if (is_node(t) && !is_wire(t)) {
                seed = std::max(seed, emit(t, sim.meta(n), n, p));
            } else if (is_support(t)) {
                seed = std::max(seed, sim.source_power(n));
            }
        }
        sim.wire_power[k] = seed;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const glm::ivec3& p : wires) {
            int& power = sim.wire_power[key_of(p)];
            for (const glm::ivec3& d : kDirs) {
                const auto it = sim.wire_power.find(key_of(p + d));
                if (it == sim.wire_power.end()) continue;
                if (it->second - 1 > power) {
                    power = it->second - 1;
                    changed = true;
                }
            }
        }
    }

    // 3. Желаемые состояния зажимателей и повторителей — все по одному снимку мира.
    struct Apply {
        glm::ivec3 pos;
        Block_Types target;
    };
    std::vector<Apply> to_apply;
    for (const Redstone_Key& k : m_nodes) {
        const glm::ivec3 p = pos_of(k);
        if (!world.loaded(p.x, p.y, p.z)) continue;
        const Block_Types t = world.type(p.x, p.y, p.z);
        const uint8_t meta = world.meta(p.x, p.y, p.z);

        Block_Types desired = t;
        int delay = 0;
        if (t == Block_Types::Signal_Clamp || t == Block_Types::Signal_Clamp_Off) {
            const bool on = sim.full_power(support_of(p, t, meta)) == 0;
            desired = on ? Block_Types::Signal_Clamp : Block_Types::Signal_Clamp_Off;
            delay = k_clamp_delay_ticks;
        } else if (t == Block_Types::Repeater || t == Block_Types::Repeater_On) {
            const glm::ivec3 out = facing_offset(meta);
            const int input = sim.repeater_input(p - out, p);
            // Блокировка: с боку в повторитель выдаёт сигнал другой включённый повторитель.
            bool locked = false;
            for (const glm::ivec3& d : kDirs) {
                if (d == out || d == -out || d.y != 0) continue;
                const glm::ivec3 s = p + d;
                if (sim.type(s) == Block_Types::Repeater_On && s + facing_offset(sim.meta(s)) == p) {
                    locked = true;
                }
            }
            if (!locked) {
                desired = input > 0 ? Block_Types::Repeater_On : Block_Types::Repeater;
                delay = repeater_delay(meta);
            }
        } else {
            continue;   // провода, рычаги и кнопки не переключаются по таймеру
        }

        if (desired == t) {
            m_pending.erase(k);
            continue;
        }
        const auto it = m_pending.find(k);
        if (it == m_pending.end() || it->second.target != desired) {
            m_pending[k] = Pending{delay, desired};
        } else if (--it->second.remaining <= 0) {
            to_apply.push_back({p, desired});
            m_pending.erase(it);
        }
    }

    // 4. Кнопки: отпускаются через k_button_ticks.
    for (auto it = m_pulses.begin(); it != m_pulses.end();) {
        const glm::ivec3 p = pos_of(it->first);
        if (!world.loaded(p.x, p.y, p.z)) { ++it; continue; }
        if (world.type(p.x, p.y, p.z) != Block_Types::Button_Pressed) {
            it = m_pulses.erase(it);
            continue;
        }
        if (--it->second <= 0) {
            to_apply.push_back({p, Block_Types::Button});
            it = m_pulses.erase(it);
        } else {
            ++it;
        }
    }

    // 5. Применить переключения.
    for (const Apply& a : to_apply) {
        write_block(world, a.pos, a.target, world.meta(a.pos.x, a.pos.y, a.pos.z));
    }
}

// --- Запись и доступ к миру -------------------------------------------------------------------

void Logic_Manager::write_block(const World_Access& world, glm::ivec3 pos, Block_Types type, uint8_t meta) {
    world.write(pos.x, pos.y, pos.z, type, meta);
    m_changed.push_back(pos);
}

std::vector<glm::ivec3> Logic_Manager::take_changed() {
    std::vector<glm::ivec3> out;
    out.swap(m_changed);
    return out;
}

namespace {
// Номер чанка по координате (с учётом отрицательных значений).
int chunk_index(int v) {
    const int s = Config::chunk_size;
    return v >= 0 ? v / s : -((-v + s - 1) / s);
}
}  // namespace

Logic_Manager::World_Access make_chunk_world(Chunk_Manager& chunks) {
    return Logic_Manager::World_Access{
        [&chunks](int x, int y, int z) { return chunks.get_block_world(x, y, z); },
        [&chunks](int x, int y, int z) { return chunks.get_block_meta_world(x, y, z); },
        [&chunks](int x, int y, int z, Block_Types t, uint8_t m) { chunks.set_block_world(x, y, z, t, m); },
        [&chunks](int x, int y, int z) {
            return chunks.is_chunk_loaded(chunk_index(x), chunk_index(y), chunk_index(z));
        }};
}
