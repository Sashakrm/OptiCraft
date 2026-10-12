#include "world/fluids/Fluid_Simulator.h"

#include "utils/Config.h"
#include "world/chunks/Chunk_Manager.h"

#include <algorithm>

namespace {
    // Четыре горизонтальных направления (порядок как у Direction.Plane.HORIZONTAL).
    constexpr glm::ivec3 k_horizontal[4] = {{0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}};
    constexpr int k_opposite[4] = {2, 3, 0, 1};
    constexpr glm::ivec3 k_down{0, -1, 0};
    constexpr glm::ivec3 k_up{0, 1, 0};
    constexpr int k_far = 1000;   // "обрыва не нашли"

    struct Cell {
        Block_Types type = Block_Types::Air;
        uint8_t meta = 0;
        bool loaded = false;
    };

    // Клетка мира; за пределами загруженных чанков и высот мира loaded = false.
    Cell read_cell(Chunk_Manager& chunks, const glm::ivec3& p) {
        Cell cell;
        if (p.y < Config::world_min_y || p.y > Config::world_max_y) return cell;
        const glm::ivec3 c = chunks.get_chunk_coords_for_world(p.x, p.y, p.z);
        Chunk* chunk = chunks.get_chunk(c.x, c.y, c.z);
        if (!chunk) return cell;
        const int lx = p.x - c.x * Config::chunk_size;
        const int ly = p.y - c.y * Config::chunk_height;
        const int lz = p.z - c.z * Config::chunk_size;
        cell.type = chunk->get_block(lx, ly, lz);
        cell.meta = chunk->get_meta(lx, ly, lz);
        cell.loaded = true;
        return cell;
    }

    bool is_liquid(Block_Types type) { return get_block_props(type).state == Block_State::Liquid; }

    // Состояние жидкости клетки. amount: источник и падающая вода — 8, течение — 8 - level (1..7).
    struct Fluid_State {
        bool empty = true;
        bool source = false;
        bool falling = false;
        int amount = 0;

        bool operator==(const Fluid_State& o) const {
            return empty == o.empty && source == o.source && falling == o.falling && amount == o.amount;
        }
    };

    Fluid_State make_source() { return {false, true, false, 8}; }
    Fluid_State make_flowing(int amount, bool falling) { return {false, false, falling, amount}; }

    Fluid_State state_of(const Cell& cell, Block_Types fluid) {
        if (!cell.loaded || cell.type != fluid) return {};
        if (cell.meta & Block_Meta::fluid_falling_bit) return make_flowing(8, true);
        const int level = cell.meta & Block_Meta::fluid_level_mask;
        if (level == 0) return make_source();
        return make_flowing(8 - level, false);
    }

    uint8_t encode(const Fluid_State& s) {
        if (s.source) return 0;
        if (s.falling) return Block_Meta::fluid_falling_bit;
        return static_cast<uint8_t>(8 - s.amount);
    }

    // Блок, который жидкость может вытеснить: воздух или мелкая растительность (её смывает).
    bool replaceable(Block_Types type) {
        if (type == Block_Types::Air) return true;
        const Block_Properties& props = get_block_props(type);
        return props.state != Block_State::Liquid && !props.is_solid &&
               props.mesh_style == Block_Mesh_Style::XStyle;
    }

    // "canSpreadTo": в эту клетку можно налить жидкость.
    bool can_spread_to(const Cell& cell) { return cell.loaded && replaceable(cell.type); }

    // "isWaterHole": клетка под клеткой — пропасть для течения (воздух/растение или вода того же вида).
    bool is_hole(const Cell& below, Block_Types fluid) {
        if (!below.loaded) return false;
        return below.type == fluid || replaceable(below.type);
    }

    // "canPassThrough": через клетку жидкость может течь горизонтально (источники не пропускают).
    bool pass_through(const Cell& cell, Block_Types fluid) {
        if (!cell.loaded) return false;
        if (cell.type == fluid) return !state_of(cell, fluid).source;
        return replaceable(cell.type);
    }

    // "getNewLiquid": какой жидкостью клетка должна стать по своим соседям.
    Fluid_State compute_new_state(Chunk_Manager& chunks, const glm::ivec3& pos, Block_Types fluid,
                                  const Block_Properties& params) {
        int strongest = 0;
        int sources = 0;
        for (const glm::ivec3& d : k_horizontal) {
            const Fluid_State s = state_of(read_cell(chunks, pos + d), fluid);
            if (s.empty) continue;
            if (s.source) ++sources;
            strongest = std::max(strongest, s.amount);
        }

        if (params.fluid_infinite && sources >= 2) {
            const Cell below = read_cell(chunks, pos + k_down);
            if (below.loaded &&
                (get_block_props(below.type).is_solid || state_of(below, fluid).source)) {
                return make_source();
            }
        }

        if (!state_of(read_cell(chunks, pos + k_up), fluid).empty) return make_flowing(8, true);

        const int amount = strongest - params.fluid_drop_off;
        if (amount <= 0) return {};
        return make_flowing(amount, false);
    }

    int source_neighbors(Chunk_Manager& chunks, const glm::ivec3& pos, Block_Types fluid) {
        int n = 0;
        for (const glm::ivec3& d : k_horizontal) {
            if (state_of(read_cell(chunks, pos + d), fluid).source) ++n;
        }
        return n;
    }

    // "getSlopeDistance": число шагов до ближайшего обрыва в этом направлении (k_far — нет).
    int slope_distance(Chunk_Manager& chunks, const glm::ivec3& pos, int distance, int from_dir,
                       Block_Types fluid, const Block_Properties& params) {
        int best = k_far;
        for (int d = 0; d < 4; ++d) {
            if (d == from_dir) continue;
            const glm::ivec3 np = pos + k_horizontal[d];
            if (!pass_through(read_cell(chunks, np), fluid)) continue;
            if (is_hole(read_cell(chunks, np + k_down), fluid)) return distance;
            if (distance < params.fluid_slope_distance) {
                best = std::min(best, slope_distance(chunks, np, distance + 1, k_opposite[d],
                                                     fluid, params));
            }
        }
        return best;
    }

    // Проставляет жидкость в клетку и назначает ей тик (замена сообщает соседям через notify_changed).
    void place(Chunk_Manager& chunks, Fluid_Simulator& sim, const glm::ivec3& pos,
               Block_Types fluid, const Fluid_State& state, const Block_Properties& params) {
        chunks.set_block_world(pos.x, pos.y, pos.z, fluid, encode(state));
        sim.schedule(pos, params.fluid_tick_delay);
    }

    // "spreadToSides": растекание в стороны (только к ближайшим обрывам, если они есть).
    void spread_to_sides(Chunk_Manager& chunks, Fluid_Simulator& sim, const glm::ivec3& pos,
                         const Fluid_State& state, Block_Types fluid, const Block_Properties& params) {
        const int next_amount = state.falling ? 7 : state.amount - params.fluid_drop_off;
        if (next_amount <= 0) return;

        int best = k_far + 1;
        int chosen[4];
        int chosen_count = 0;
        for (int d = 0; d < 4; ++d) {
            const glm::ivec3 np = pos + k_horizontal[d];
            if (!pass_through(read_cell(chunks, np), fluid)) continue;

            const int distance = is_hole(read_cell(chunks, np + k_down), fluid)
                ? 0
                : slope_distance(chunks, np, 1, k_opposite[d], fluid, params);
            if (distance < best) chosen_count = 0;
            if (distance <= best) {
                chosen[chosen_count++] = d;
                best = distance;
            }
        }

        for (int i = 0; i < chosen_count; ++i) {
            const glm::ivec3 np = pos + k_horizontal[chosen[i]];
            const Cell target = read_cell(chunks, np);
            if (!can_spread_to(target)) continue;
            const Fluid_State fresh = compute_new_state(chunks, np, fluid, params);
            if (fresh.empty) continue;
            place(chunks, sim, np, fluid, fresh, params);
        }
    }

    // "spread": сначала вниз; в стороны — если вниз нельзя (или клетка — источник).
    void spread(Chunk_Manager& chunks, Fluid_Simulator& sim, const glm::ivec3& pos,
                const Fluid_State& state, Block_Types fluid, const Block_Properties& params) {
        const glm::ivec3 below_pos = pos + k_down;
        const Cell below = read_cell(chunks, below_pos);
        const Fluid_State below_state = compute_new_state(chunks, below_pos, fluid, params);

        if (can_spread_to(below) && !below_state.empty) {
            place(chunks, sim, below_pos, fluid, below_state, params);
            if (source_neighbors(chunks, pos, fluid) >= 3) {
                spread_to_sides(chunks, sim, pos, state, fluid, params);
            }
        } else if (state.source || !is_hole(below, fluid)) {
            spread_to_sides(chunks, sim, pos, state, fluid, params);
        }
    }

    void tick_cell(Chunk_Manager& chunks, Fluid_Simulator& sim, const glm::ivec3& pos) {
        const Cell cell = read_cell(chunks, pos);
        if (!cell.loaded || !is_liquid(cell.type)) return;

        const Block_Types fluid = cell.type;
        const Block_Properties& params = get_block_props(fluid);
        Fluid_State state = state_of(cell, fluid);

        if (!state.source) {
            const Fluid_State fresh = compute_new_state(chunks, pos, fluid, params);
            if (fresh.empty) {
                chunks.set_block_world(pos.x, pos.y, pos.z, Block_Types::Air);   // течение отступило
                return;
            }
            if (!(fresh == state)) {
                chunks.set_block_world(pos.x, pos.y, pos.z, fluid, encode(fresh));
                sim.schedule(pos, params.fluid_tick_delay);
                state = fresh;
            }
        }
        spread(chunks, sim, pos, state, fluid, params);
    }
}

void Fluid_Simulator::schedule(const glm::ivec3& position, int delay_ticks) {
    const uint64_t due = m_tick + static_cast<uint64_t>(std::max(1, delay_ticks));
    const auto it = m_scheduled.find(position);
    if (it != m_scheduled.end() && it->second <= due) return;   // уже назначено раньше
    m_scheduled[position] = due;
    m_queue.push({due, m_sequence++, position});
}

void Fluid_Simulator::notify_changed(Chunk_Manager& chunks, const glm::ivec3& position) {
    static constexpr glm::ivec3 k_around[7] = {
        {0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
    };
    for (const glm::ivec3& d : k_around) {
        const glm::ivec3 p = position + d;
        const Cell cell = read_cell(chunks, p);
        if (cell.loaded && is_liquid(cell.type)) {
            schedule(p, get_block_props(cell.type).fluid_tick_delay);
        }
    }
}

void Fluid_Simulator::update(Chunk_Manager& chunks, float delta_time) {
    if (m_scheduled.empty()) {
        m_accumulator = 0.0f;   // без работы время не копим: иначе после паузы был бы всплеск тиков
        return;
    }

    m_accumulator = std::min(m_accumulator + delta_time, 0.5f);
    int budget = Config::fluid_max_updates_per_frame;

    while (m_accumulator >= Config::fluid_tick_seconds) {
        m_accumulator -= Config::fluid_tick_seconds;
        ++m_tick;

        while (!m_queue.empty() && m_queue.top().due <= m_tick && budget > 0) {
            const Entry entry = m_queue.top();
            m_queue.pop();

            const auto it = m_scheduled.find(entry.position);
            if (it == m_scheduled.end() || it->second != entry.due) continue;   // устаревшая запись
            m_scheduled.erase(it);

            tick_cell(chunks, *this, entry.position);
            --budget;
        }
        if (budget <= 0) break;   // остаток досчитаем в следующем кадре (сроки просрочены — ничего не теряется)
    }
}

void Fluid_Simulator::clear() {
    m_queue = {};
    m_scheduled.clear();
    m_accumulator = 0.0f;
}
