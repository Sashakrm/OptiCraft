#include "entities/items/Dropped_Item.h"
#include "world/chunks/Chunk_Manager.h"
#include "world/blocks/Block_Types.h"
#include "math/Math_Helpers.h"

#include <algorithm>
#include <cmath>

namespace {
    constexpr float k_half = Config::dropped_item_size * 0.5f;
    // Размеры именно КАРТИНКИ (хитбокс — Config::dropped_item_size): в Minecraft предмет на земле
    // тоже нарисован крупнее своей коллизии.
    constexpr float k_render_cube = 0.30f;
    constexpr float k_render_sprite = 0.45f;
    constexpr float k_collect_duration = 0.15f;
    constexpr float k_merge_interval = 0.25f;

    // Куб рисуется только для «настоящих» блоков; предметы и крестообразную флору (цветы, трава)
    // показываем плоским спрайтом — так же, как это делает Minecraft.
    bool draws_as_sprite(const Block_Properties& props) {
        return props.is_item || props.mesh_style == Block_Mesh_Style::XStyle;
    }

    glm::vec3 rotate_y(const glm::vec3& v, float cos_yaw, float sin_yaw) {
        return glm::vec3(v.x * cos_yaw + v.z * sin_yaw, v.y, -v.x * sin_yaw + v.z * cos_yaw);
    }

    void emit_quad(std::vector<Chunk_Vertex>& out,
                   const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
                   const glm::vec3& normal, const glm::vec4& uv, float light) {
        // uv = (u_min, v_min, u_max, v_max): p0 — левый низ спрайта, p2 — правый верх.
        const Chunk_Vertex v0{p0.x, p0.y, p0.z, uv.x, uv.y, normal.x, normal.y, normal.z, light};
        const Chunk_Vertex v1{p1.x, p1.y, p1.z, uv.z, uv.y, normal.x, normal.y, normal.z, light};
        const Chunk_Vertex v2{p2.x, p2.y, p2.z, uv.z, uv.w, normal.x, normal.y, normal.z, light};
        const Chunk_Vertex v3{p3.x, p3.y, p3.z, uv.x, uv.w, normal.x, normal.y, normal.z, light};
        out.push_back(v0); out.push_back(v1); out.push_back(v2);
        out.push_back(v0); out.push_back(v2); out.push_back(v3);
    }
}

Item_Stack roll_block_drop(Block_Types block, std::mt19937& rng) {
    const Block_Properties& props = get_block_props(block);
    if (props.drop_id == 0 || props.drop_count <= 0) return {};

    if (props.drop_chance < 1.0f) {
        std::uniform_real_distribution<float> chance(0.0f, 1.0f);
        if (chance(rng) >= props.drop_chance) return {};
    }

    const Block_Types dropped = props.drop_id < 0 ? block : static_cast<Block_Types>(props.drop_id);
    if (dropped == Block_Types::Air) return {};
    return Item_Stack{dropped, props.drop_count};
}

Dropped_Item_Manager::Dropped_Item_Manager()
    : m_rng(std::random_device{}()) {
}

void Dropped_Item_Manager::spawn(const Item_Stack& stack, const glm::vec3& position,
                                 const glm::vec3& velocity, float pickup_delay) {
    if (stack.is_empty()) return;

    std::uniform_real_distribution<float> phase_dist(0.0f, 6.2831853f);

    Dropped_Item item;
    item.stack = stack;
    item.position = position;
    item.velocity = velocity;
    item.pickup_delay = pickup_delay;
    item.phase = phase_dist(m_rng);
    m_items.push_back(item);

    enforce_limit();
}

void Dropped_Item_Manager::spawn_block_drops(const glm::ivec3& block_pos, Block_Types block,
                                             const std::vector<Item_Stack>& container_contents) {
    std::uniform_real_distribution<float> offset(-0.2f, 0.2f);
    std::uniform_real_distribution<float> speed(-1.6f, 1.6f);

    // Центр блока, чуть ниже середины: хитбокс предмета целиком внутри клетки сломанного блока.
    const glm::vec3 center(static_cast<float>(block_pos.x) + 0.5f,
                           static_cast<float>(block_pos.y) + 0.5f - k_half,
                           static_cast<float>(block_pos.z) + 0.5f);

    auto pop = [&](const Item_Stack& stack) {
        if (stack.is_empty()) return;
        spawn(stack,
              center + glm::vec3(offset(m_rng), 0.0f, offset(m_rng)),
              glm::vec3(speed(m_rng), 3.5f, speed(m_rng)));
    };

    pop(roll_block_drop(block, m_rng));
    for (const Item_Stack& stack : container_contents) {
        pop(stack);
    }
}

void Dropped_Item_Manager::spawn_from_mob(const glm::vec3& mob_position, const Item_Stack& stack) {
    std::uniform_real_distribution<float> speed(-1.2f, 1.2f);
    spawn(stack,
          mob_position + glm::vec3(0.0f, 0.4f, 0.0f),
          glm::vec3(speed(m_rng), 4.0f, speed(m_rng)));
}

bool Dropped_Item_Manager::collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const {
    constexpr float epsilon = 0.001f;

    const int min_x = static_cast<int>(std::floor(position.x - k_half + epsilon));
    const int max_x = static_cast<int>(std::floor(position.x + k_half - epsilon));
    const int min_y = static_cast<int>(std::floor(position.y + epsilon));
    const int max_y = static_cast<int>(std::floor(position.y + Config::dropped_item_size - epsilon));
    const int min_z = static_cast<int>(std::floor(position.z - k_half + epsilon));
    const int max_z = static_cast<int>(std::floor(position.z + k_half - epsilon));

    for (int y = min_y; y <= max_y; ++y) {
        for (int z = min_z; z <= max_z; ++z) {
            for (int x = min_x; x <= max_x; ++x) {
                if (get_block_props(chunk_manager.get_block_world(x, y, z)).is_solid) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool Dropped_Item_Manager::is_chunk_loaded_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const {
    const int wx = static_cast<int>(std::floor(position.x));
    const int wy = static_cast<int>(std::floor(position.y));
    const int wz = static_cast<int>(std::floor(position.z));
    return chunk_manager.get_chunk(
        Math_Helpers::floor_div(wx, Config::chunk_size),
        Math_Helpers::floor_div(wy, Config::chunk_height),
        Math_Helpers::floor_div(wz, Config::chunk_size)) != nullptr;
}

void Dropped_Item_Manager::simulate(Dropped_Item& item, float delta_time, const Chunk_Manager& chunk_manager) {
    // Оказался внутри твёрдого блока (например, игрок поставил блок прямо на предмет) —
    // выталкиваем вверх, иначе он застрял бы в стене навсегда.
    if (collides_at(item.position, chunk_manager)) {
        for (int i = 0; i < 8 && collides_at(item.position, chunk_manager); ++i) {
            item.position.y += 0.25f;
        }
        item.velocity = glm::vec3(0.0f);
    }

    const Block_Types block_at_center = chunk_manager.get_block_world(
        static_cast<int>(std::floor(item.position.x)),
        static_cast<int>(std::floor(item.position.y + k_half)),
        static_cast<int>(std::floor(item.position.z)));
    const bool in_liquid = get_block_props(block_at_center).state == Block_State::Liquid;

    if (in_liquid) {
        // В воде предмет медленно всплывает и почти не скользит.
        item.velocity.y += (1.2f - item.velocity.y) * std::min(1.0f, 4.0f * delta_time);
        const float water_drag = std::exp(-3.0f * delta_time);
        item.velocity.x *= water_drag;
        item.velocity.z *= water_drag;
    } else {
        item.velocity.y = std::max(Config::mob_max_fall_speed,
                                   item.velocity.y + Config::mob_gravity * delta_time);
        // Трение: по земле предмет быстро останавливается, в воздухе почти не тормозит.
        const float drag = std::exp(-(item.on_ground ? 10.0f : 0.4f) * delta_time);
        item.velocity.x *= drag;
        item.velocity.z *= drag;
    }
    if (std::abs(item.velocity.x) < 0.01f) item.velocity.x = 0.0f;
    if (std::abs(item.velocity.z) < 0.01f) item.velocity.z = 0.0f;

    // Быстро падающий предмет за один кадр может пролететь несколько блоков и «просочиться»
    // сквозь тонкий пол — двигаем его подшагами не длиннее ~0.2 блока.
    const float max_move = std::max({std::abs(item.velocity.x), std::abs(item.velocity.y),
                                     std::abs(item.velocity.z)}) * delta_time;
    const int steps = std::clamp(static_cast<int>(std::ceil(max_move / 0.2f)), 1, 12);
    const float h = delta_time / static_cast<float>(steps);

    bool touched_ground = false;
    for (int i = 0; i < steps; ++i) {
        glm::vec3 target = item.position;
        target.x += item.velocity.x * h;
        if (collides_at(target, chunk_manager)) item.velocity.x = 0.0f; else item.position.x = target.x;

        target = item.position;
        target.z += item.velocity.z * h;
        if (collides_at(target, chunk_manager)) item.velocity.z = 0.0f; else item.position.z = target.z;

        target = item.position;
        target.y += item.velocity.y * h;
        if (collides_at(target, chunk_manager)) {
            if (item.velocity.y < 0.0f) touched_ground = true;
            item.velocity.y = 0.0f;
        } else {
            item.position.y = target.y;
        }
    }
    item.on_ground = touched_ground;
}

void Dropped_Item_Manager::try_pickup(Dropped_Item& item, const glm::vec3& player_position,
                                      float player_height, Inventory& inventory) {
    if (item.pickup_delay > 0.0f || item.collecting) return;

    const float dx = item.position.x - player_position.x;
    const float dz = item.position.z - player_position.z;
    constexpr float radius = Config::dropped_item_pickup_radius;
    if (dx * dx + dz * dz > radius * radius) return;

    // По вертикали — от чуть ниже ног до чуть выше макушки.
    if (item.position.y + Config::dropped_item_size < player_position.y - 0.4f ||
        item.position.y > player_position.y + player_height + 0.2f) {
        return;
    }

    const int accepted = inventory.try_add_stack(item.stack);
    if (accepted <= 0) return; // инвентарь полон — предмет остаётся лежать

    if (accepted >= item.stack.count) {
        item.collecting = true;
        item.collect_time = 0.0f;
        item.collect_from = item.position;
    } else {
        item.stack.count -= accepted; // влезла только часть — остаток лежит дальше
    }
}

void Dropped_Item_Manager::merge_nearby() {
    constexpr float radius_sq = Config::dropped_item_merge_radius * Config::dropped_item_merge_radius;

    for (size_t i = 0; i < m_items.size(); ++i) {
        Dropped_Item& a = m_items[i];
        if (a.collecting || a.stack.is_empty() || a.stack.count >= max_stack_for(a.stack.type)) continue;

        for (size_t j = i + 1; j < m_items.size(); ++j) {
            Dropped_Item& b = m_items[j];
            if (b.collecting || b.stack.is_empty() || b.stack.type != a.stack.type) continue;
            if (a.stack.damage != b.stack.damage) continue;
            if (a.stack.count + b.stack.count > max_stack_for(a.stack.type)) continue;

            const glm::vec3 d = a.position - b.position;
            if (glm::dot(d, d) > radius_sq) continue;

            // Как в Minecraft: слитый предмет получает возраст более «свежего» — так стопка,
            // в которую что-то подсыпали, не исчезает раньше времени.
            a.stack.count += b.stack.count;
            a.age = std::min(a.age, b.age);
            a.pickup_delay = std::min(a.pickup_delay, b.pickup_delay);
            b.stack = Item_Stack{};
        }
    }

    m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
                                 [](const Dropped_Item& item) { return item.stack.is_empty(); }),
                  m_items.end());
}

void Dropped_Item_Manager::enforce_limit() {
    while (static_cast<int>(m_items.size()) > Config::dropped_item_max_count) {
        const auto oldest = std::max_element(m_items.begin(), m_items.end(),
            [](const Dropped_Item& lhs, const Dropped_Item& rhs) { return lhs.age < rhs.age; });
        m_items.erase(oldest);
    }
}

void Dropped_Item_Manager::update(float delta_time,
                                  const Chunk_Manager& chunk_manager,
                                  const glm::vec3& player_position,
                                  float player_height,
                                  Inventory* pickup_inventory) {
    m_time += delta_time;
    m_collect_target = player_position + glm::vec3(0.0f, player_height * 0.6f, 0.0f);

    for (Dropped_Item& item : m_items) {
        if (item.collecting) {
            item.collect_time += delta_time;
            continue;
        }

        // Предмет в незагруженном чанке замирает и не стареет: иначе он либо провалился бы
        // сквозь ещё не сгенерированную землю, либо исчез, пока игрок далеко.
        if (!is_chunk_loaded_at(item.position, chunk_manager)) continue;

        item.age += delta_time;
        item.pickup_delay = std::max(0.0f, item.pickup_delay - delta_time);

        simulate(item, delta_time, chunk_manager);

        if (pickup_inventory) {
            try_pickup(item, player_position, player_height, *pickup_inventory);
        }
    }

    m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
        [](const Dropped_Item& item) {
            if (item.collecting) return item.collect_time >= k_collect_duration;
            if (item.age >= Config::dropped_item_lifetime) return true;
            return item.position.y < static_cast<float>(Config::world_min_y) - 8.0f; // упал в пустоту
        }), m_items.end());

    m_merge_timer += delta_time;
    if (m_merge_timer >= k_merge_interval) {
        m_merge_timer = 0.0f;
        merge_nearby();
    }
}

void Dropped_Item_Manager::build_mesh(std::vector<Chunk_Vertex>& out, const Chunk_Manager& chunk_manager) const {
    // Смещения копий в стаке (как в Minecraft: чем больше стак, тем «толще» кучка).
    static const glm::vec3 k_copy_offsets[3] = {
        glm::vec3(0.0f), glm::vec3(0.09f, 0.0f, 0.07f), glm::vec3(-0.08f, 0.0f, -0.09f)
    };

    for (const Dropped_Item& item : m_items) {
        if (item.stack.is_empty()) continue;
        const Block_Properties& props = get_block_props(item.stack.type);
        const bool sprite = draws_as_sprite(props);
        const float render_half = (sprite ? k_render_sprite : k_render_cube) * 0.5f;

        const float t = m_time + item.phase;
        const float yaw = t * Config::dropped_item_spin_speed;
        const float cos_yaw = std::cos(yaw);
        const float sin_yaw = std::sin(yaw);

        // Покачивание вверх-вниз, как в Minecraft (0 .. 0.08 блока над землёй).
        const float bob = 0.04f + 0.04f * std::sin(t * 2.6f);
        glm::vec3 center = item.position + glm::vec3(0.0f, render_half + 0.02f + bob, 0.0f);
        float scale = 1.0f;

        if (item.collecting) {
            // Летит к игроку и уменьшается — визуальный «всасывающий» эффект подбора.
            const float k = std::clamp(item.collect_time / k_collect_duration, 0.0f, 1.0f);
            const glm::vec3 from = item.collect_from + glm::vec3(0.0f, render_half, 0.0f);
            center = glm::mix(from, m_collect_target, k * k);
            scale = 1.0f - 0.5f * k;
        }

        // Свет берём из клетки предмета; у не загруженного чанка вернётся 0 (рисуется тускло).
        const float light = static_cast<float>(chunk_manager.get_packed_light_world(
            static_cast<int>(std::floor(center.x)),
            static_cast<int>(std::floor(center.y)),
            static_cast<int>(std::floor(center.z))));

        const int copies = item.stack.count <= 1 ? 1 : (item.stack.count <= 16 ? 2 : 3);
        const float h = render_half * scale;

        for (int copy = 0; copy < copies; ++copy) {
            const glm::vec3 c = center + k_copy_offsets[copy];

            if (sprite) {
                // Плоский двусторонний спрайт (GL_CULL_FACE на время рисования предметов выключен).
                const glm::vec3 right = rotate_y(glm::vec3(h, 0.0f, 0.0f), cos_yaw, sin_yaw);
                const glm::vec3 up(0.0f, h, 0.0f);
                const glm::vec3 normal = rotate_y(glm::vec3(0.0f, 0.0f, 1.0f), cos_yaw, sin_yaw);
                emit_quad(out, c - right - up, c + right - up, c + right + up, c - right + up,
                          normal, props.uv_top, light);
                continue;
            }

            auto P = [&](float x, float y, float z) {
                return c + rotate_y(glm::vec3(x, y, z) * h, cos_yaw, sin_yaw);
            };
            auto N = [&](float x, float y, float z) {
                return rotate_y(glm::vec3(x, y, z), cos_yaw, sin_yaw);
            };
            const glm::vec4& side = props.uv_side;
            emit_quad(out, P(-1,-1, 1), P( 1,-1, 1), P( 1, 1, 1), P(-1, 1, 1), N(0, 0, 1), side, light);
            emit_quad(out, P( 1,-1,-1), P(-1,-1,-1), P(-1, 1,-1), P( 1, 1,-1), N(0, 0,-1), side, light);
            emit_quad(out, P( 1,-1, 1), P( 1,-1,-1), P( 1, 1,-1), P( 1, 1, 1), N(1, 0, 0), side, light);
            emit_quad(out, P(-1,-1,-1), P(-1,-1, 1), P(-1, 1, 1), P(-1, 1,-1), N(-1,0, 0), side, light);
            emit_quad(out, P(-1, 1, 1), P( 1, 1, 1), P( 1, 1,-1), P(-1, 1,-1), N(0, 1, 0), props.uv_top, light);
            emit_quad(out, P(-1,-1,-1), P( 1,-1,-1), P( 1,-1, 1), P(-1,-1, 1), N(0,-1, 0), props.uv_bottom, light);
        }
    }
}
