#include "world/blocks/Block_Interaction.h"
#include "world/blocks/Block_Shapes.h"
#include <glad/gl.h>
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Entity.h"
#include "input/Input_Manager.h"
#include "rendering/Renderer.h"
#include "entities/player/Player.h"
#include "entities/player/Inventory.h"
#include "entities/animals/Pig.h"
#include "entities/items/Dropped_Item.h"
#include "utils/Config.h"
#include "math/Math_Helpers.h"
#include "world/crafting/Crop_Manager.h"
#include "world/crafting/Sapling_Manager.h"
#include "world/logic/Logic_Manager.h"

#include <algorithm>
#include <cmath>

namespace {
    constexpr int MOUSE_BUTTON_LEFT = 0;
    constexpr int MOUSE_BUTTON_RIGHT = 1;

    void invalidate_chunk_and_neighbors(int wx, int wy, int wz,
                                        Chunk_Manager& chunk_manager,
                                        Renderer& renderer) {
        renderer.invalidate_chunk_cache(chunk_manager.get_chunk_coords_for_world(wx, wy, wz));

        const int lx = wx - Math_Helpers::floor_div(wx, Config::chunk_size) * Config::chunk_size;
        const int ly = wy - Math_Helpers::floor_div(wy, Config::chunk_height) * Config::chunk_height;
        const int lz = wz - Math_Helpers::floor_div(wz, Config::chunk_size) * Config::chunk_size;

        if (lx == 0) {
            renderer.invalidate_chunk_cache(chunk_manager.get_chunk_coords_for_world(wx - 1, wy, wz));
        } else if (lx == Config::chunk_size - 1) {
            renderer.invalidate_chunk_cache(chunk_manager.get_chunk_coords_for_world(wx + 1, wy, wz));
        }

        if (ly == 0) {
            renderer.invalidate_chunk_cache(chunk_manager.get_chunk_coords_for_world(wx, wy - 1, wz));
        } else if (ly == Config::chunk_height - 1) {
            renderer.invalidate_chunk_cache(chunk_manager.get_chunk_coords_for_world(wx, wy + 1, wz));
        }

        if (lz == 0) {
            renderer.invalidate_chunk_cache(chunk_manager.get_chunk_coords_for_world(wx, wy, wz - 1));
        } else if (lz == Config::chunk_size - 1) {
            renderer.invalidate_chunk_cache(chunk_manager.get_chunk_coords_for_world(wx, wy, wz + 1));
        }
    }

    // Пересечение луча с AABB (slab-метод). Возвращает расстояние вдоль луча или -1, если промах.
    float ray_vs_aabb(const glm::vec3& origin, const glm::vec3& dir,
                      const glm::vec3& box_min, const glm::vec3& box_max) {
        float t_near = 0.0f;
        float t_far = 1e9f;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(dir[axis]) < 1e-6f) {
                if (origin[axis] < box_min[axis] || origin[axis] > box_max[axis]) return -1.0f;
                continue;
            }
            float t1 = (box_min[axis] - origin[axis]) / dir[axis];
            float t2 = (box_max[axis] - origin[axis]) / dir[axis];
            if (t1 > t2) std::swap(t1, t2);
            t_near = std::max(t_near, t1);
            t_far = std::min(t_far, t2);
            if (t_near > t_far) return -1.0f;
        }
        return t_near;
    }
}

void invalidate_block_mesh(int wx, int wy, int wz, Chunk_Manager& chunk_manager, Renderer& renderer) {
    invalidate_chunk_and_neighbors(wx, wy, wz, chunk_manager, renderer);
}

Block_Interaction::Block_Interaction() = default;

bool Block_Interaction::can_harvest(Block_Types block, const Item_Stack& held) {
    const Block_Properties& props = get_block_props(block);
    if (props.harvest_tier <= 0) return true;
    if (held.is_empty()) return false;
    const Block_Properties& tool = get_block_props(held.type);
    return tool.tool_type == Tool_Type::Pickaxe && tool.tool_tier >= props.harvest_tier;
}

float Block_Interaction::get_survival_break_time(Block_Types block, const Item_Stack& held) {
    const Block_Properties& props = get_block_props(block);
    float time = props.break_time >= 0.0f
        ? props.break_time
        : std::max(0.0f, props.hardness) * Config::survival_break_seconds_per_hardness;
    if (!held.is_empty()) {
        const Block_Properties& tool = get_block_props(held.type);
        if (tool.tool_type != Tool_Type::None && tool.tool_type == props.effective_tool) {
            time /= std::max(1.0f, tool.tool_speed);
        }
    }
    // Руду без нужной кирки ломать долго (и дропа не будет).
    if (props.harvest_tier > 0 && !can_harvest(block, held)) time *= 3.0f;
    return time;
}

void Block_Interaction::cancel_actions() {
    m_is_mining = false;
    m_mining_time = 0.0f;
    m_break_progress = 0.0f;
    m_eat_timer = 0.0f;
    m_eat_progress = 0.0f;
    m_eating_type = Block_Types::Air;
}

void Block_Interaction::update(float delta_time,
                               const glm::vec3& eye_position,
                               const glm::vec3& look_direction,
                               const Input_Manager& input,
                               Player& player,
                               Chunk_Manager& chunk_manager,
                               Renderer& renderer,
                               Inventory& inventory,
                               float entity_hit_distance) {
    m_action_cooldown = std::max(0.0f, m_action_cooldown - delta_time);
    m_current_hit = Raycast::cast(eye_position, look_direction, k_max_reach, chunk_manager);

    // Моб ближе первого блока на луче — он перехватывает взгляд.
    m_entity_targeted = entity_hit_distance >= 0.0f &&
                        (!m_current_hit.hit || entity_hit_distance < m_current_hit.distance);
    if (m_entity_targeted) {
        m_current_hit.hit = false;
    }

    const bool survival = inventory.get_mode() == Game_Mode::Survival;

    // --- Еда (Survival): ПКМ удерживается на съедобном предмете в активном слоте -------------
    // Работает и когда игрок смотрит в небо (нет блока на луче), поэтому до проверки цели.
    {
        const Item_Stack held = inventory.get_hotbar_slot(static_cast<size_t>(inventory.get_selected_index()));
        const Block_Properties& held_props = get_block_props(held.type);
        const bool aimed_at_container = m_current_hit.hit &&
            block_entity_type_for(m_current_hit.block_type) != Block_Entity_Type::None;
        // Морковь на грядке сажается, а не съедается; моб на луче кормится (см. Game).
        const bool aimed_at_farmland = m_current_hit.hit && held.type == Block_Types::Carrot &&
            Crop_Manager::is_farmland(m_current_hit.block_type);
        const bool wants_to_eat = survival && !held.is_empty() && held_props.food_points > 0 &&
                                  player.can_eat() && player.is_alive() && !aimed_at_container &&
                                  !aimed_at_farmland && !m_entity_targeted &&
                                  input.is_mouse_button_held(MOUSE_BUTTON_RIGHT);

        if (wants_to_eat && (m_eat_timer == 0.0f || m_eating_type == held.type)) {
            m_eating_type = held.type;
            m_eat_timer += delta_time;
            m_eat_progress = std::clamp(m_eat_timer / Config::eat_duration, 0.0f, 1.0f);
            if (m_eat_timer >= Config::eat_duration) {
                if (inventory.remove_resource(held.type, 1)) {
                    player.eat(held_props.food_points, held_props.food_saturation);
                }
                // Следующая порция начинается заново — как в Minecraft, нужно снова держать ПКМ.
                m_eat_timer = 0.0f;
                m_eat_progress = 0.0f;
                m_eating_type = Block_Types::Air;
                m_action_cooldown = k_action_delay;
            }
        } else {
            m_eat_timer = 0.0f;
            m_eat_progress = 0.0f;
            m_eating_type = Block_Types::Air;
        }
    }

    // Пока ест — блоки не ломаем и не ставим.
    if (m_eat_progress > 0.0f) {
        m_is_mining = false;
        m_break_progress = 0.0f;
        return;
    }

    // --- Добыча ------------------------------------------------------------------------------
    const bool left_held = input.is_mouse_button_held(MOUSE_BUTTON_LEFT);

    if (survival) {
        // Смотрим на другой блок (или никуда) / отпустили кнопку — прогресс сбрасывается,
        // как в Minecraft: недокопанный блок «зарастает».
        const bool same_target = m_current_hit.hit &&
            m_mining_pos == glm::ivec3(m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z) &&
            m_mining_type == m_current_hit.block_type;
        if (!left_held || !m_current_hit.hit || !same_target) {
            m_is_mining = false;
            m_mining_time = 0.0f;
            m_break_progress = 0.0f;
        }

        if (left_held && m_current_hit.hit && m_action_cooldown <= 0.0f) {
            if (!m_is_mining) {
                m_is_mining = true;
                m_mining_pos = glm::ivec3(m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z);
                m_mining_type = m_current_hit.block_type;
                m_mining_time = 0.0f;
            }
            m_mining_time += delta_time;

            const Item_Stack held_stack = inventory.get_selected_stack();
            const float required = get_survival_break_time(m_mining_type, held_stack);
            m_break_progress = required > 0.0f ? std::clamp(m_mining_time / required, 0.0f, 1.0f) : 1.0f;

            if (m_mining_time >= required) {
                Broken_Block_Event event;
                event.position = m_mining_pos;
                event.type = m_mining_type;
                event.harvested = can_harvest(m_mining_type, held_stack);
                // Содержимое сундука/печи/верстака снимаем сейчас: set_block_world ниже удалит
                // Block_Entity вместе с блоком. Своя половина двойного сундука — только своя.
                if (const Block_Entity* entity = chunk_manager.get_block_entity(
                        m_mining_pos.x, m_mining_pos.y, m_mining_pos.z)) {
                    for (const Item_Stack& stack : entity->slots) {
                        if (!stack.is_empty()) event.container_contents.push_back(stack);
                    }
                }

                if (chunk_manager.set_block_world(m_mining_pos.x, m_mining_pos.y, m_mining_pos.z,
                                                  Block_Types::Air)) {
                    m_broken_blocks.push_back(std::move(event));
                    player.add_exhaustion(Config::exhaustion_break_block);
                    // Инструмент в руке изнашивается (на добыче «пустышек» вроде травы — нет).
                    if (get_block_props(m_mining_type).hardness > 0.0f) {
                        inventory.damage_selected_tool(1);
                    }
                    invalidate_chunk_and_neighbors(m_mining_pos.x, m_mining_pos.y, m_mining_pos.z,
                                                   chunk_manager, renderer);
                }
                m_is_mining = false;
                m_mining_time = 0.0f;
                m_break_progress = 0.0f;
                m_action_cooldown = k_break_cooldown;
                m_current_hit.hit = false;
                return;
            }
        }
    } else {
        m_is_mining = false;
        m_break_progress = 0.0f;
    }

    if (!m_current_hit.hit || m_action_cooldown > 0.0f) {
        return;
    }

    // Creative: мгновенная поломка по клику, как и раньше.
    if (!survival && input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)) {
        if (!chunk_manager.set_block_world(
            m_current_hit.block_x,
            m_current_hit.block_y,
            m_current_hit.block_z,
            Block_Types::Air
        )) {
            return;
        }
        invalidate_chunk_and_neighbors(
            m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z,
            chunk_manager, renderer);
        m_action_cooldown = k_action_delay;
        m_current_hit.hit = false;
        return;
    }

    if (input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)) {
        // Блок, на который сейчас смотрит игрок (а не клетка, куда встал бы новый блок) —
        // если это контейнер, ПКМ его открывает, как в Minecraft, а не ставит блок ему на грань.
        if (block_entity_type_for(m_current_hit.block_type) != Block_Entity_Type::None) {
            m_container_open_request = glm::ivec3(
                m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z);
            m_action_cooldown = k_action_delay;
            m_current_hit.hit = false;
            return;
        }

        // Рычаг, кнопка, зажиматель, повторитель: ПКМ переключает/меняет, блок не ставится.
        // Провод — нет: по нему ставят другие блоки.
        if (Logic_Manager::is_node(m_current_hit.block_type) &&
            !Logic_Manager::is_wire(m_current_hit.block_type)) {
            m_redstone_interact = glm::ivec3(m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z);
            m_action_cooldown = k_action_delay;
            m_current_hit.hit = false;
            return;
        }

        // --- Земледелие -----------------------------------------------------------------------
        {
            const Block_Properties& held_props = get_block_props(m_place_block);
            const glm::ivec3 target(m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z);
            const glm::ivec3 above = target + glm::ivec3(0, 1, 0);
            const bool top_face = m_current_hit.place_x == above.x && m_current_hit.place_y == above.y &&
                                  m_current_hit.place_z == above.z;
            const Block_Types above_block = chunk_manager.get_block_world(above.x, above.y, above.z);
            const bool above_free = above_block == Block_Types::Air ||
                                    (!get_block_props(above_block).is_solid &&
                                     get_block_props(above_block).state != Block_State::Liquid &&
                                     !Crop_Manager::is_crop_block(above_block));

            // Мотыга: трава/земля -> грядка.
            if (held_props.tool_type == Tool_Type::Hoe &&
                (m_current_hit.block_type == Block_Types::Grass || m_current_hit.block_type == Block_Types::Dirt) &&
                above_free) {
                const Block_Types farmland = Crop_Manager::has_water_nearby(chunk_manager, target)
                    ? Block_Types::Farmland_Wet : Block_Types::Farmland;
                if (chunk_manager.set_block_world(target.x, target.y, target.z, farmland)) {
                    // Трава над грядкой (цветок/травинка) сносится, иначе висела бы в воздухе.
                    if (above_block != Block_Types::Air) {
                        chunk_manager.set_block_world(above.x, above.y, above.z, Block_Types::Air);
                    }
                    invalidate_chunk_and_neighbors(target.x, target.y, target.z, chunk_manager, renderer);
                    if (survival) inventory.damage_selected_tool(1);
                    m_action_cooldown = k_action_delay;
                }
                return;
            }

            // Семена / морковь: посадка на грядку сверху.
            const bool plantable_item = m_place_block == Block_Types::Wheat_Seeds || m_place_block == Block_Types::Carrot;
            if (plantable_item && m_crop_manager &&
                (m_current_hit.block_type == Block_Types::Farmland || m_current_hit.block_type == Block_Types::Farmland_Wet)) {
                if (top_face && above_free && !player.intersects_block_aabb(above.x, above.y, above.z)) {
                    const Crop_Kind kind = m_place_block == Block_Types::Carrot ? Crop_Kind::Carrot : Crop_Kind::Wheat;
                    if (chunk_manager.set_block_world(above.x, above.y, above.z, Crop_Manager::crop_block(kind, 0))) {
                        m_crop_manager->plant(above, kind);
                        if (survival) inventory.consume_selected(1);
                        invalidate_chunk_and_neighbors(above.x, above.y, above.z, chunk_manager, renderer);
                        m_action_cooldown = k_action_delay;
                    }
                }
                return;
            }
        }

        // Предметы (мясо, уголь) — не блоки: в мир они не ставятся.
        if (m_place_block == Block_Types::Air || get_block_props(m_place_block).is_item) {
            return;
        }

        // Саженец ели ставится только сверху на землю или дёрн.
        if (Sapling_Manager::is_sapling(m_place_block)) {
            const bool on_top = m_current_hit.place_x == m_current_hit.block_x &&
                                m_current_hit.place_y == m_current_hit.block_y + 1 &&
                                m_current_hit.place_z == m_current_hit.block_z;
            if (!on_top || !Sapling_Manager::can_grow_on(m_current_hit.block_type)) {
                return;
            }
        }

        const glm::vec3 place_pos(
            static_cast<float>(m_current_hit.place_x),
            static_cast<float>(m_current_hit.place_y),
            static_cast<float>(m_current_hit.place_z)
        );

        const float dx = eye_position.x - place_pos.x;
        const float dy = eye_position.y - place_pos.y;
        const float dz = eye_position.z - place_pos.z;
        const float dist_sq = dx * dx + dy * dy + dz * dz;

        if (dist_sq > 1.75f * 1.75f &&
            !player.intersects_block_aabb(
                m_current_hit.place_x, m_current_hit.place_y, m_current_hit.place_z)) {
            // Survival: нечего ставить — пустой/несовпадающий слот хотбара.
            if (survival && !inventory.has_resource(m_place_block, 1)) {
                return;
            }
            // Метаданные при постановке: бревно ложится вдоль оси грани, на которую его
            // поставили (на бок соседнего блока -> ствол горизонтально), а печь/сундук/верстак
            // разворачиваются лицом к игроку.
            uint8_t placed_meta = 0;
            const Block_Properties& place_props = get_block_props(m_place_block);
            if (place_props.orientation == Block_Orientation::Axis) {
                const int nx = m_current_hit.place_x - m_current_hit.block_x;
                const int nz = m_current_hit.place_z - m_current_hit.block_z;
                if (nx != 0)      placed_meta = Block_Meta::axis_x;
                else if (nz != 0) placed_meta = Block_Meta::axis_z;
                else              placed_meta = Block_Meta::axis_y;
            } else if (place_props.orientation == Block_Orientation::Facing) {
                const float to_player_x = eye_position.x - (place_pos.x + 0.5f);
                const float to_player_z = eye_position.z - (place_pos.z + 0.5f);
                if (std::abs(to_player_x) > std::abs(to_player_z)) {
                    placed_meta = to_player_x > 0.0f ? Block_Meta::facing_east : Block_Meta::facing_west;
                } else {
                    placed_meta = to_player_z > 0.0f ? Block_Meta::facing_south : Block_Meta::facing_north;
                }
                // Ступеньки: высокая сторона — от игрока (по ней идут вверх, к игроку — вниз).
                if (place_props.shape == Block_Shape::Stairs) {
                    placed_meta = opposite_facing(placed_meta);
                }
            }

            // Красная логика: рычаг/кнопка/зажиматель — на грань опоры, повторитель — выходом от игрока,
            // провод — на твёрдую опору снизу. Без опоры не ставится (см. Logic_Manager::placement).
            const bool is_redstone_node = Logic_Manager::is_node(m_place_block);
            if (is_redstone_node) {
                const float to_player_x = eye_position.x - (place_pos.x + 0.5f);
                const float to_player_z = eye_position.z - (place_pos.z + 0.5f);
                uint8_t toward_player = Block_Meta::facing_north;
                if (std::abs(to_player_x) > std::abs(to_player_z)) {
                    toward_player = to_player_x > 0.0f ? Block_Meta::facing_east : Block_Meta::facing_west;
                } else {
                    toward_player = to_player_z > 0.0f ? Block_Meta::facing_south : Block_Meta::facing_north;
                }
                auto world = make_chunk_world(chunk_manager);
                uint8_t node_meta = 0;
                if (!Logic_Manager::placement(
                        m_place_block, world,
                        glm::ivec3(m_current_hit.place_x, m_current_hit.place_y, m_current_hit.place_z),
                        glm::ivec3(m_current_hit.block_x, m_current_hit.block_y, m_current_hit.block_z),
                        opposite_facing(toward_player), node_meta)) {
                    return;
                }
                placed_meta = node_meta;
            }

            if (!chunk_manager.set_block_world(
                m_current_hit.place_x,
                m_current_hit.place_y,
                m_current_hit.place_z,
                m_place_block,
                placed_meta
            )) {
                return;
            }
            if (is_redstone_node) {
                m_redstone_placed = glm::ivec3(m_current_hit.place_x, m_current_hit.place_y, m_current_hit.place_z);
            }
            if (survival) {
                inventory.remove_resource(m_place_block, 1);
            }
            if (m_sapling_manager && Sapling_Manager::is_sapling(m_place_block)) {
                m_sapling_manager->plant(glm::ivec3(m_current_hit.place_x, m_current_hit.place_y, m_current_hit.place_z),
                                         m_place_block);
            }
            invalidate_chunk_and_neighbors(
                m_current_hit.place_x, m_current_hit.place_y, m_current_hit.place_z,
                chunk_manager, renderer);
            m_action_cooldown = k_action_delay;
        }
    }
}
