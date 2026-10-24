//
// Created by noktemor on 14.10.2026.
//

//
// Mobs_Update.cpp — реализация модуля мобов. Описание назначения и компромиссов — в Mobs_Update.h.
//
#include "mobs/Mobs_Update.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <random>

#include "utils/Config.h"
#include "utils/logging/Logger.h"
#include "world/chunks/Chunk_Manager.h"
#include "rendering/Shader.h"
#include "world/generation/World_Generator.h"

#include <optional>

namespace {
    // Группы мобов с разными правилами появления.
    enum class Spawn_Group { Animal, Horse, Hostile };

    // Биомы, где появляются мобы. Вода, небо, солнечные острова и подземелья исключены: мобы
    // появляются только на земле. Список правится здесь.
    bool biome_allows(Spawn_Group group, Biome_Id biome) {
        switch (group) {
            case Spawn_Group::Animal:
                switch (biome) {
                    case Biome_Id::Plains:
                    case Biome_Id::Forest:
                    case Biome_Id::Birch_Forest:
                    case Biome_Id::Taiga:
                    case Biome_Id::Jungle:
                    case Biome_Id::Savanna:
                    case Biome_Id::Steppe:
                        return true;
                    default:
                        return false;
                }
            case Spawn_Group::Horse:
                return biome == Biome_Id::Plains || biome == Biome_Id::Steppe || biome == Biome_Id::Savanna;
            case Spawn_Group::Hostile:
                switch (biome) {
                    case Biome_Id::Ocean:
                    case Biome_Id::Shallow_Water:
                    case Biome_Id::Sky:
                    case Biome_Id::Solar:
                    case Biome_Id::Underground:
                    case Biome_Id::Deep_Underground:
                    case Biome_Id::Flooded_Underground:
                        return false;
                    default:
                        return true;
                }
        }
        return false;
    }

    Biome_Id biome_at(const Chunk_Manager& chunks, int x, int y, int z) {
        const World_Generator* generator = chunks.world_generator();
        return generator ? generator->biome_id(x, y, z) : Biome_Id::Plains;
    }

    // Точка на поверхности земли в кольце [rmin, rmax] вокруг center. Нужны: твёрдый блок
    // (для животных и лошадей — трава), два свободных блока над ним не в воде и подходящий биом.
    // Незагруженный чанк, вода, неподходящий биом — пустой результат, попробуем в другой раз.
    std::optional<glm::vec3> find_ground_spawn(const glm::vec3& center, float rmin, float rmax,
                                               Spawn_Group group, bool grass_only, float height_above_ground,
                                               Chunk_Manager& chunks, std::mt19937& rng) {
        std::uniform_real_distribution<float> angle_dist(0.0f, 6.2831853f);
        std::uniform_real_distribution<float> radius_dist(rmin, rmax);
        const float angle = angle_dist(rng);
        const float radius = radius_dist(rng);
        const float x = center.x + std::cos(angle) * radius;
        const float z = center.z + std::sin(angle) * radius;

        const int bx = static_cast<int>(std::floor(x));
        const int bz = static_cast<int>(std::floor(z));
        const int top = static_cast<int>(std::floor(center.y)) + 12;
        const int bottom = static_cast<int>(std::floor(center.y)) - 24;
        for (int y = top; y >= bottom; --y) {
            const Block_Types ground = chunks.get_block_world(bx, y, bz);
            if (grass_only ? ground != Block_Types::Grass : !get_block_props(ground).is_solid) continue;
            const Block_Properties& above1 = get_block_props(chunks.get_block_world(bx, y + 1, bz));
            const Block_Properties& above2 = get_block_props(chunks.get_block_world(bx, y + 2, bz));
            if (above1.is_solid || above2.is_solid ||
                above1.state == Block_State::Liquid || above2.state == Block_State::Liquid) continue;
            if (!biome_allows(group, biome_at(chunks, bx, y, bz))) return std::nullopt;
            return glm::vec3(x, static_cast<float>(y) + height_above_ground, z);
        }
        return std::nullopt;
    }

    // Сколько раз за один вызов пробуем найти место (каждая проба — поиск по столбу и биому).
    constexpr int k_spawn_attempts = 4;

    // Пересечение луча с AABB (slab-метод). Возвращает расстояние до входа в коробку или -1.
    float ray_aabb_distance(const glm::vec3& origin, const glm::vec3& dir,
                            const glm::vec3& box_min, const glm::vec3& box_max, float max_distance) {
        float t_min = 0.0f;
        float t_max = max_distance;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(dir[axis]) < 1e-6f) {
                if (origin[axis] < box_min[axis] || origin[axis] > box_max[axis]) return -1.0f;
                continue;
            }
            float t1 = (box_min[axis] - origin[axis]) / dir[axis];
            float t2 = (box_max[axis] - origin[axis]) / dir[axis];
            if (t1 > t2) std::swap(t1, t2);
            t_min = std::max(t_min, t1);
            t_max = std::min(t_max, t2);
            if (t_min > t_max) return -1.0f;
        }
        return t_min;
    }

    // Видно ли точку to из точки from: луч не должен пересекать непрозрачные блоки.
    // Шаг 0.5 блока — достаточно, чтобы не проскочить тонкую стену.
    bool has_line_of_sight(const Chunk_Manager& chunks, const glm::vec3& from, const glm::vec3& to) {
        const glm::vec3 delta = to - from;
        const float length = glm::length(delta);
        const int steps = std::max(1, static_cast<int>(length / 0.5f));
        for (int i = 1; i < steps; ++i) {
            const glm::vec3 p = from + delta * (static_cast<float>(i) / static_cast<float>(steps));
            const Block_Types block = chunks.get_block_world(
            static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), static_cast<int>(std::floor(p.z)));
            if (get_block_props(block).is_solid) return false;
        }
        return true;
    }
    // Общая логика размножения для любого вида (Pig/Passive_Animal/Horse имеют breeding(), get_position(),
    // seek_towards() и is_dying()/kill()). Возвращает пары, у которых родился детёныш.
    template <class Animal>
    std::vector<std::pair<Animal*, Animal*>> find_breeding_pairs(std::vector<std::unique_ptr<Animal>>& animals) {
        std::vector<std::pair<Animal*, Animal*>> born;
        for (size_t i = 0; i < animals.size(); ++i) {
            Animal& a = *animals[i];
            if (a.is_dying() || !a.breeding().in_love()) continue;

            // Ближайший влюблённый партнёр того же вида в радиусе поиска.
            Animal* partner = nullptr;
            float best = Config::animal_breed_search_radius;
            for (size_t j = 0; j < animals.size(); ++j) {
                if (i == j) continue;
                Animal& b = *animals[j];
                if (b.is_dying() || !b.breeding().in_love()) continue;
                const float d = glm::length(glm::vec2(b.get_position().x - a.get_position().x,
                                                      b.get_position().z - a.get_position().z));
                if (d < best) { best = d; partner = &b; }
            }
            if (!partner) continue;

            a.seek_towards(partner->get_position());
            // Пара считается один раз (индекс i < индекса партнёра).
            if (best < 1.6f && &a < partner) born.emplace_back(&a, partner);
        }
        return born;
    }
    int index_of(Passive_Kind kind) { return static_cast<int>(kind); }
    int index_of(Hostile_Kind kind) { return static_cast<int>(kind); }
}

// --- Жизненный цикл ---------------------------------------------------------------------------

void Mobs_Update::clear() {
    m_pigs.clear();
    m_cows.clear();
    m_sheep.clear();
    m_horses.clear();
    m_hostiles.clear();
    m_ridden_horse = nullptr;
    m_ride_input = Horse_Ride_Input{};
    m_dismount_requested = false;
    m_target_kind = Target_Kind::None;
    m_target_pig = nullptr;
    m_target_passive = nullptr;
    m_target_horse = nullptr;
    m_target_passive = nullptr;
}

void Mobs_Update::shutdown() {
    clear();
    // Mob::load() кэширует Mob_Model в статической мапе. Очистка списков не убивает модель, пока
    // жив кэш, поэтому кэш сбрасывается здесь, пока GL-контекст ещё жив (см. Mob.h::clear_model_cache).
    Mob::clear_model_cache();
}

void Mobs_Update::reset_timers() {
    m_attack_cooldown = 0.0f;
    m_pig_respawn_cooldown = 0.0f;
    m_passive_respawn_cooldown = {0.0f, 0.0f};
    m_hostile_respawn_cooldown = {0.0f, 0.0f, 0.0f};
    m_horse_respawn_cooldown = 0.0f;
}

void Mobs_Update::set_hostiles_enabled(bool enabled) {
    m_hostiles_enabled = enabled;
    if (!enabled) {
        m_hostiles.clear();
        m_target_hostile = nullptr;
        if (m_target_kind == Target_Kind::Hostile) m_target_kind = Target_Kind::None;
    }
}

std::vector<std::unique_ptr<Passive_Animal>>& Mobs_Update::passive_list(Passive_Kind kind) {
    return kind == Passive_Kind::Sheep ? m_sheep : m_cows;
}

// --- Тики ------------------------------------------------------------------------------------

Mob_Result Mobs_Update::update_tick(float dt, const Mob_Player_View& player, Chunk_Manager& chunks) {
    Mob_Result result;
    m_pig_respawn_cooldown = std::max(0.0f, m_pig_respawn_cooldown - dt);
    spawn_missing_pigs(player.position, chunks);
    for (auto& pig : m_pigs) {
        pig->update(dt, chunks);
    }

    for (Passive_Kind kind : {Passive_Kind::Cow, Passive_Kind::Sheep}) {
        const int i = index_of(kind);
        m_passive_respawn_cooldown[i] = std::max(0.0f, m_passive_respawn_cooldown[i] - dt);
        spawn_missing_passive(kind, player.position, chunks);
        for (auto& animal : passive_list(kind)) {
            animal->update(dt, chunks);
        }
}

    update_breeding();

    // Убитые животные исчезают, когда доиграна анимация падения (мясо выпало ещё в момент удара).
    m_pigs.erase(std::remove_if(m_pigs.begin(), m_pigs.end(),
                     [](const std::unique_ptr<Pig>& pig) { return pig->is_removable(); }),
                 m_pigs.end());

    for (Passive_Kind kind : {Passive_Kind::Cow, Passive_Kind::Sheep}) {
        auto& list = passive_list(kind);
        list.erase(std::remove_if(list.begin(), list.end(),
        [](const std::unique_ptr<Passive_Animal>& animal) { return animal->is_removable(); }),
        list.end());
    }
    return result;
}

// --- Враги -----------------------------------------------------------------------------------

void Mobs_Update::update_hostiles(float dt, const Mob_Player_View& player, Chunk_Manager& chunks, Mob_Result& result) {
    for (Hostile_Kind kind : {Hostile_Kind::Zombie, Hostile_Kind::Skeleton, Hostile_Kind::Spider}) {
        const int i = index_of(kind);
        m_hostile_respawn_cooldown[i] = std::max(0.0f, m_hostile_respawn_cooldown[i] - dt);
        if (m_hostiles_enabled) spawn_missing_hostiles(kind, player.position, chunks);
    }
    if (m_hostiles.empty()) return;

    // 1) Кто видит игрока: дистанция (вдвое меньше, если он крадётся) и луч через блоки.
    //    Пауки игрока видят только ночью.
    const glm::vec3 player_eye = player.position + glm::vec3(0.0f, player.eye_height, 0.0f);
    std::vector<bool> seen(m_hostiles.size(), false);
    for (size_t i = 0; i < m_hostiles.size(); ++i) {
        const Hostile_Mob& hostile = *m_hostiles[i];
        if (hostile.is_dying() || !player.alive) continue;
        if (hostile.kind() == Hostile_Kind::Spider && !player.night) continue;
        const float range = hostile.stats().detect_range * (player.sneaking ? 0.5f : 1.0f);
        const glm::vec3 d = player.position - hostile.get_position();
        const float horizontal = glm::length(glm::vec2(d.x, d.z));
        if (horizontal > range || std::abs(d.y) > range * 0.5f) continue;
        seen[i] = has_line_of_sight(chunks, hostile.get_eye(), player_eye);
    }
    // 2) Тревога: увидевший игрока поднимает врагов рядом — они тоже идут к нему (толпой).
    for (size_t i = 0; i < m_hostiles.size(); ++i) {
        if (!seen[i]) continue;
        const glm::vec3 from = m_hostiles[i]->get_position();
        for (size_t j = 0; j < m_hostiles.size(); ++j) {
            if (i == j || m_hostiles[j]->is_dying()) continue;
            const glm::vec3 d = m_hostiles[j]->get_position() - from;
            if (glm::length(glm::vec2(d.x, d.z)) <= Config::hostile_alert_radius) {
                m_hostiles[j]->alert(player.position);
            }
        }
    }

    // 3) Добыча пауков: ближайшая живая лошадь в радиусе обнаружения паука.
    std::vector<Horse*> spider_prey(m_hostiles.size(), nullptr);
    for (size_t i = 0; i < m_hostiles.size(); ++i) {
        const Hostile_Mob& hostile = *m_hostiles[i];
        if (hostile.is_dying() || hostile.kind() != Hostile_Kind::Spider) continue;
        float best = hostile.stats().detect_range;
        for (const auto& horse : m_horses) {
            if (horse->is_dying()) continue;
            const glm::vec3 d = horse->get_position() - hostile.get_position();
            const float dist = glm::length(glm::vec2(d.x, d.z));
            if (dist <= best) {
                best = dist;
                spider_prey[i] = horse.get();
            }
        }
    }

    // 4) Обновление: погоня, удар в упор, блуждание. Удар по лошади — отдельно от удара по игроку.
    Hostile_Context context;
    context.player_position = player.position;
    context.player_alive = player.alive;
    context.player_speed = player.horizontal_speed;
    for (size_t i = 0; i < m_hostiles.size(); ++i) {
        Hostile_Mob& hostile = *m_hostiles[i];
        context.player_seen = seen[i];
        context.player_targetable = hostile.kind() != Hostile_Kind::Spider || player.night;
        Horse* prey = spider_prey[i];
        context.prey_valid = prey != nullptr;
        if (prey) {
            context.prey_min = prey->get_hitbox_min();
            context.prey_max = prey->get_hitbox_max();
            context.prey_position = 0.5f * (context.prey_min + context.prey_max);
        }
        const Hostile_Result r = hostile.update(dt, chunks, context);
        result.player_damage += r.player_damage;
        if (r.hit_horse && prey && !prey->is_dying()) {
            if (prey->take_damage(r.horse_damage, hostile.get_position())) {
                m_horse_respawn_cooldown = Config::horse_respawn_delay;
            }
        }
    }

    m_hostiles.erase(std::remove_if(m_hostiles.begin(), m_hostiles.end(),
                         [](const std::unique_ptr<Hostile_Mob>& hostile) { return hostile->is_removable(); }),
                     m_hostiles.end());
}

// --- По кадрам: лошади и езда (КОМПРОМИСС) ---------------------------------------------------

Mob_Result Mobs_Update::update_frame(float dt, const Mob_Player_View& player, const Mob_Blocked_Fn& blocked,
                                     Chunk_Manager& chunks) {
    Mob_Result result;
    Mob_Player_View current = player;

    if (m_ridden_horse) {
        // Игрока унесло с седла без нашего ведома (/tp, респавн после смерти, смена мира) или
        // лошадь погибла — слезаем там, где он оказался.
        if (!current.alive || m_ridden_horse->is_dying() ||
            glm::length(current.position - m_last_seat_position) > 3.0f) {
            result = dismount(false, current, blocked);
        } else if (m_dismount_requested) {
            result = dismount(true, current, blocked);
        }
    }
    m_dismount_requested = false;
    if (result.set_player_position) current.position = result.player_position;

    spawn_missing_horses(current.position, chunks);

    Horse_Context context;
    context.player_position = current.position;
    context.player_sneaking = current.sneaking;
    context.player_alive = current.alive;
    const Horse_Ride_Input no_input{};

    // Событие «нажали Ctrl» (sprint_toggle) действует один кадр: забираем его и сбрасываем.
    const Horse_Ride_Input ride = m_ride_input;
    m_ride_input.sprint_toggle = false;

    // Лошади бьют пауков, которые рядом (и без всадника). Удар применяется здесь же.
    for (auto& horse : m_horses) {
        Horse_Context horse_context = context;
        Hostile_Mob* prey = nullptr;
        if (!horse->is_dying()) {
            float best = Config::horse_hunt_range;
            for (const auto& hostile : m_hostiles) {
                if (hostile->is_dying() || hostile->kind() != Hostile_Kind::Spider) continue;
                const glm::vec3 d = hostile->get_position() - horse->get_position();
                const float dist = glm::length(glm::vec2(d.x, d.z));
                if (dist <= best) {
                    best = dist;
                    prey = hostile.get();
                }
            }
        }
        if (prey) {
            horse_context.prey_valid = true;
            horse_context.prey_min = prey->get_hitbox_min();
            horse_context.prey_max = prey->get_hitbox_max();
        }
        horse->update(dt, chunks, horse_context, horse.get() == m_ridden_horse ? ride : no_input);
        const Horse_Attack_Result hit = horse->consume_attack();
        if (hit.hit && prey && !prey->is_dying() && prey->take_damage(hit.damage, horse->get_position())) {
            m_hostile_respawn_cooldown[index_of(Hostile_Kind::Spider)] = Config::hostile_respawn_delay;
        }
    }

    if (m_ridden_horse) {
        if (m_ridden_horse->consume_throw_request()) {
            // Дикая лошадь сбросила всадника.
            result = dismount(true, current, blocked);
        } else {
            // Седло тянет игрока за собой: позиция всадника = позиция седла.
            m_last_seat_position = m_ridden_horse->get_seat_position();
            result.set_player_position = true;
            result.player_position = m_last_seat_position;
        }
    }

    // Убитые лошади исчезают, когда доиграна анимация падения.
    m_horses.erase(std::remove_if(m_horses.begin(), m_horses.end(),
                       [this](const std::unique_ptr<Horse>& horse) {
                           if (!horse->is_removable()) return false;
                           if (horse.get() == m_ridden_horse) m_ridden_horse = nullptr;
                           return true;
                       }),
                   m_horses.end());
    m_horse_respawn_cooldown = std::max(0.0f, m_horse_respawn_cooldown - dt);
    return result;
}

Mob_Result Mobs_Update::dismount(bool place_beside, const Mob_Player_View& player, const Mob_Blocked_Fn& blocked) {
    Mob_Result result;
    Horse* horse = m_ridden_horse;
    if (!horse) return result;
    m_ridden_horse = nullptr;

    glm::vec3 position = player.position;
    if (place_beside) {
        // Ставим рядом с лошадью на свободное место: слева, справа, сзади, спереди.
        const glm::vec3 horse_position = horse->get_position();
        const float yaw = glm::radians(horse->get_yaw_degrees());
        const glm::vec3 forward(std::sin(yaw), 0.0f, std::cos(yaw));
        const glm::vec3 left(std::cos(yaw), 0.0f, -std::sin(yaw));
        const glm::vec3 offsets[] = { left * 1.3f, -left * 1.3f, -forward * 1.6f, forward * 1.6f };

        bool found = false;
        for (const glm::vec3& offset : offsets) {
            const glm::vec3 candidate = horse_position + offset + glm::vec3(0.0f, 0.05f, 0.0f);
            if (!blocked(candidate)) {
                position = candidate;
                found = true;
                break;
            }
        }
        if (!found) position = horse_position + glm::vec3(0.0f, Config::horse_height + 0.1f, 0.0f);
    }

    horse->dismount(position);
    m_ride_input = Horse_Ride_Input{};
    result.set_player_position = true;
    result.player_position = position;
    result.set_riding = true;
    result.riding = false;
    return result;
}

Mob_Result Mobs_Update::mount(Horse& horse, const Mob_Player_View& player) {
    Mob_Result result;
    if (m_ridden_horse || !player.alive) return result;
    if (!horse.mount()) return result;

    m_ridden_horse = &horse;
    m_last_seat_position = horse.get_seat_position();
    m_dismount_requested = false;

    result.set_riding = true;
    result.riding = true;
    result.cancel_block_actions = true;
    result.set_player_position = true;
    result.player_position = m_last_seat_position;
    return result;
}

// --- Отладка (F3+F4) -------------------------------------------------------------------------

Mobs_Update::Mob_Debug_Counts Mobs_Update::debug_counts() const {
    const auto alive = [](const auto& list) {
        int n = 0;
        for (const auto& mob : list) {
            if (!mob->is_dying()) ++n;
        }
        return n;
    };
    Mob_Debug_Counts counts;
    counts.hostile = alive(m_hostiles);
    counts.passive = alive(m_pigs) + alive(m_cows) + alive(m_sheep) + alive(m_horses);
    return counts;
}

namespace {

std::string format_float(const char* fmt, float value) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), fmt, value);
    return buffer;
}

std::string describe_breeding(const Breeding_State& breeding) {
    if (breeding.is_baby()) return "baby, grows up in " + format_float("%.0f", breeding.grow_left) + " s";
    std::string text = "adult";
    if (breeding.in_love()) text += ", in love (" + format_float("%.0f", breeding.love_left) + " s)";
    if (breeding.cooldown_left > 0.0f) text += ", breeding cooldown " + format_float("%.0f", breeding.cooldown_left) + " s";
    return text;
}

std::string describe_hunger(int hunger) {
    if (hunger >= 50) return "hungry";
    if (hunger <= 10) return "full";
    return "satisfied";
}

const char* passive_kind_name(Passive_Kind kind) {
    return kind == Passive_Kind::Cow ? "Cow" : "Sheep";
}

const char* hostile_kind_name(Hostile_Kind kind) {
    switch (kind) {
        case Hostile_Kind::Zombie: return "Zombie";
        case Hostile_Kind::Skeleton: return "Skeleton";
        case Hostile_Kind::Spider: return "Spider";
    }
    return "Hostile";
}

}  // namespace

std::vector<std::string> Mobs_Update::debug_target_lines() const {
    std::vector<std::string> lines;
    if (m_target_kind == Target_Kind::None) {
        lines.emplace_back("Target: none (look at an animal or monster)");
        return lines;
    }

    switch (m_target_kind) {
        case Target_Kind::Pig: {
            const Pig& pig = *m_target_pig;
            const Pig_AI& ai = pig.get_ai();
            lines.push_back("Target: Pig \"" + ai.get_name() + "\"");
            lines.push_back("HP: " + format_float("%.1f", pig.get_health()) + " / " + format_float("%.1f", Config::pig_max_health));
            lines.push_back("State: " + ai.get_state_name());
            lines.push_back("Hunger: " + std::to_string(ai.get_hunger()) + " (" + describe_hunger(ai.get_hunger()) + ")"
                            + "  Energy: " + std::to_string(ai.get_energy())
                            + "  Happiness: " + std::to_string(ai.get_happiness()));
            lines.push_back("Age: " + describe_breeding(m_target_pig->breeding()));
            if (pig.is_dying()) lines.emplace_back("Dying");
            break;
        }
        case Target_Kind::Passive: {
            const Passive_Animal& animal = *m_target_passive;
            lines.push_back(std::string("Target: ") + passive_kind_name(animal.kind()));
            lines.push_back("HP: " + format_float("%.1f", animal.get_health()) + " / " + format_float("%.1f", animal.stats().max_health));
            lines.push_back(std::string("Panicking: ") + (animal.is_panicking() ? "yes" : "no"));
            lines.push_back("Age: " + describe_breeding(m_target_passive->breeding()));
            if (animal.is_dying()) lines.emplace_back("Dying");
            break;
        }
        case Target_Kind::Horse: {
            const Horse& horse = *m_target_horse;
            const Horse_AI& ai = horse.get_ai();
            lines.emplace_back("Target: Horse \"" + ai.get_name() + "\"");
            lines.push_back("HP: " + format_float("%.1f", horse.get_health()) + " / " + format_float("%.1f", Config::horse_max_health));
            lines.push_back("State: " + ai.get_state_name());
            lines.push_back("Hunger: " + format_float("%.0f", static_cast<float>(ai.get_hunger())) + " (" + describe_hunger(static_cast<int>(ai.get_hunger())) + ")"
                            + "  Energy: " + format_float("%.0f", ai.get_energy())
                            + "  Fear: " + format_float("%.0f", ai.get_fear()));
            lines.push_back(std::string("Tamed: ") + (horse.is_tamed() ? "yes" : "no")
                            + (horse.is_tamed() ? "" : " (progress " + format_float("%.0f", horse.get_taming_progress() * 100.0f) + "%)")
                            + "  Rider: " + (horse.has_rider() ? "yes" : "no"));
            lines.push_back("Age: " + describe_breeding(m_target_horse->breeding()));
            if (horse.is_dying()) lines.emplace_back("Dying");
            break;
        }
        case Target_Kind::Hostile: {
            const Hostile_Mob& hostile = *m_target_hostile;
            lines.push_back(std::string("Target: ") + hostile_kind_name(hostile.kind()));
            lines.push_back("HP: " + format_float("%.1f", hostile.get_health()) + " / " + format_float("%.1f", hostile.stats().max_health));
            lines.push_back(std::string("Aggro: ") + (hostile.is_aggro() ? "yes" : "no")
                            + "  Attack cooldown: " + format_float("%.1f", hostile.get_attack_cooldown()) + " s");
            if (hostile.is_dying()) lines.emplace_back("Dying");
            break;
        }
        case Target_Kind::None:
            break;
    }
    return lines;
}

// --- Взаимодействие игрока (по кадрам) -------------------------------------------------------


float Mobs_Update::aim(const glm::vec3& eye, const glm::vec3& look_dir) {
    m_target_kind = Target_Kind::None;
    m_target_pig = nullptr;
    m_target_passive = nullptr;
    m_target_horse = nullptr;
    m_target_hostile = nullptr;
    float best = -1.0f;
    const auto consider = [&](float d, Target_Kind kind, Pig* pig, Passive_Animal* passive,
                              Horse* horse, Hostile_Mob* hostile) {
        if (d >= 0.0f && (best < 0.0f || d < best)) {
            best = d;
            m_target_kind = kind;
            m_target_pig = pig;
            m_target_passive = passive;
            m_target_horse = horse;
            m_target_hostile = hostile;
        }
    };

    for (auto& pig : m_pigs) {
        if (pig->is_dying()) continue;
        const float d = ray_aabb_distance(eye, look_dir, pig->get_hitbox_min(), pig->get_hitbox_max(),
                                          Config::player_attack_reach);
        consider(d, Target_Kind::Pig, pig.get(), nullptr, nullptr, nullptr);
    }
    for (auto* list : {&m_cows, &m_sheep}) {
        for (auto& animal : *list) {
            if (animal->is_dying()) continue;
            const float d = ray_aabb_distance(eye, look_dir, animal->get_hitbox_min(), animal->get_hitbox_max(),
                Config::player_attack_reach);
            consider(d, Target_Kind::Passive, nullptr, animal.get(), nullptr, nullptr);
        }
    }
    // Лошадь, на которой сидим, не целим — хитбокс вокруг камеры мешал бы.
    for (auto& horse : m_horses) {
        if (horse->is_dying() || horse.get() == m_ridden_horse) continue;
        const float d = ray_aabb_distance(eye, look_dir, horse->get_hitbox_min(), horse->get_hitbox_max(),
                                          Config::player_attack_reach);
        consider(d, Target_Kind::Horse, nullptr, nullptr, horse.get(), nullptr);
    }
    for (auto& hostile : m_hostiles) {
        if (hostile->is_dying()) continue;
        const float d = ray_aabb_distance(eye, look_dir, hostile->get_hitbox_min(), hostile->get_hitbox_max(),
        Config::player_attack_reach);
        consider(d, Target_Kind::Hostile, nullptr, nullptr, nullptr, hostile.get());
    }
    return best;
}

Mob_Result Mobs_Update::interact(const Mob_Interact_Input& input, Dropped_Item_Manager& drops) {
    Mob_Result result;
    m_attack_cooldown = std::max(0.0f, m_attack_cooldown - input.delta_time);

    if (!input.block_targets_entity || m_target_kind == Target_Kind::None) return result;
    const bool survival = input.survival;

    // --- Кормление (ПКМ едой, которую животное ест) ----------------------------------------
    if (input.right_click) {
        bool fed = false;
        if (m_target_pig && Pig::accepts_food(input.held_block) && m_target_pig->breeding().can_be_fed()) {
            m_target_pig->breeding().start_love();
            fed = true;
        } else if (m_target_passive && m_target_passive->accepts_food(input.held_block) &&
            m_target_passive->breeding().can_be_fed()) {
            m_target_passive->breeding().start_love();
            fed = true;
        } else if (m_target_horse && Horse::accepts_food(input.held_block) && m_target_horse->feed(input.held_block)) {
            fed = true;
            if (input.chat) input.chat(std::string("Horse ate: ") + get_block_props(input.held_block).name);
        }

        if (fed) {
            result.consume_held_item = survival;
            result.cancel_block_actions = true;
        } else if (m_target_horse && !m_ridden_horse && !m_target_horse->is_baby()) {
            // Не еда — садимся верхом (жеребёнка оседлать нельзя).
            result = mount(*m_target_horse, input.player);
        }
    }

    // --- Удар (ЛКМ) -------------------------------------------------------------------------
    if (input.left_click && m_attack_cooldown <= 0.0f) {
        // Оружие/инструмент в руке задаёт свою перезарядку и свой коэффициент скорости; кулак — по Config.
        const Block_Properties* held_props = nullptr;
        if (survival && !input.held_stack.is_empty()) held_props = &get_block_props(input.held_stack.type);
        const bool armed = held_props && held_props->attack_damage > 0.0f;

        m_attack_cooldown = (armed && held_props->attack_cooldown >= 0.0f)
            ? held_props->attack_cooldown : Config::player_attack_cooldown;
        result.exhaustion += Config::exhaustion_attack;

        // Урон: в Creative — с одного удара; в Survival — кулак или оружие/инструмент в руке.
        float damage = Config::player_fist_damage;
        if (armed) damage = std::max(damage, held_props->attack_damage);
        // Скорость игрока добавляет урону сверх ходьбы; потолка нет. Коэффициент — у оружия.
        const float speed_scale = armed ? held_props->speed_damage_scale : 1.0f;
        const float over_walk = std::max(0.0f, input.player.horizontal_speed - Config::player_move_speed);
        damage *= 1.0f + over_walk * Config::melee_speed_bonus_per_speed * speed_scale;

        static std::mt19937 s_drop_rng(std::random_device{}());
        const auto roll = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(s_drop_rng); };

        bool hit = false;
        if (m_target_pig) {
            const float d = survival ? damage : Config::pig_max_health * 100.0f;
            hit = true;
            if (m_target_pig->take_damage(d, input.player.position)) {
                m_pig_respawn_cooldown = Config::pig_respawn_delay;
                if (survival) {
                    drops.spawn_from_mob(m_target_pig->get_position(),
                        Item_Stack{Block_Types::Raw_Porkchop, roll(Config::pig_drop_min, Config::pig_drop_max)});
                }
            }
        } else if (m_target_passive) {
            const Passive_Stats& st = m_target_passive->stats();
            const float d = survival ? damage : st.max_health * 100.0f;
            hit = true;
            if (m_target_passive->take_damage(d, input.player.position)) {
                m_passive_respawn_cooldown[index_of(m_target_passive->kind())] = st.respawn_delay;
                if (survival && st.drop_item != Block_Types::Air) {
                    drops.spawn_from_mob(m_target_passive->get_position(),
                        Item_Stack{st.drop_item, roll(st.drop_min, st.drop_max)});
                }
                if (survival && m_target_passive->kind() == Passive_Kind::Cow) {
                    drops.spawn_from_mob(m_target_passive->get_position(),
                        Item_Stack{Block_Types::Leather, roll(Config::cow_leather_min, Config::cow_leather_max)});
                }
            }
        } else if (m_target_horse) {
            const float d = survival ? damage : Config::horse_max_health * 100.0f;
            hit = true;
            if (m_target_horse->take_damage(d, input.player.position)) {
                m_horse_respawn_cooldown = Config::horse_respawn_delay;
                if (survival) {
                    drops.spawn_from_mob(m_target_horse->get_position(),
                        Item_Stack{Block_Types::Raw_Horse_Meat, roll(Config::horse_drop_min, Config::horse_drop_max)});
                    drops.spawn_from_mob(m_target_horse->get_position(),
                        Item_Stack{Block_Types::Leather, roll(Config::horse_leather_min, Config::horse_leather_max)});
                }
            }
        } else if (m_target_hostile) {
            const Hostile_Stats& st = m_target_hostile->stats();
            const float d = survival ? damage : st.max_health * 100.0f;
            hit = true;
            if (m_target_hostile->take_damage(d, input.player.position)) {
                m_hostile_respawn_cooldown[index_of(m_target_hostile->kind())] = Config::hostile_respawn_delay;
            }
        }
        if (hit && survival) result.damage_held_tool = true;
    }
    return result;
}

// --- Команды ---------------------------------------------------------------------------------

int Mobs_Update::kill_all() {
    int killed = 0;
    for (auto& pig : m_pigs) {
        if (pig->kill()) ++killed;
    }
    // Иначе spawn_missing_pigs тут же досоздаст новых — команда выглядела бы бесполезной.
    if (killed > 0) m_pig_respawn_cooldown = Config::pig_respawn_delay;

    int killed_horses = 0;
    for (auto& horse : m_horses) {
        if (horse->kill()) ++killed_horses;
    }
    if (killed_horses > 0) m_horse_respawn_cooldown = Config::horse_respawn_delay;
    int killed_passive = 0;
    for (Passive_Kind kind : {Passive_Kind::Cow, Passive_Kind::Sheep}) {
        int count = 0;
        for (auto& animal : passive_list(kind)) {
            if (animal->kill()) ++count;
        }
        if (count > 0) m_passive_respawn_cooldown[index_of(kind)] = passive_stats(kind).respawn_delay;
        killed_passive += count;
    }

    int killed_hostiles = 0;
    for (auto& hostile : m_hostiles) {
        if (hostile->kill()) ++killed_hostiles;
    }
    if (killed_hostiles > 0) {
        m_hostile_respawn_cooldown = {Config::hostile_respawn_delay, Config::hostile_respawn_delay, Config::hostile_respawn_delay};
    }
    return killed + killed_horses + killed_passive + killed_hostiles;
}
int Mobs_Update::summon_passive(Passive_Kind kind, int count, const glm::vec3& around) {
    static int s_summon_counter = 0;
    static std::mt19937 s_rng(std::random_device{}());
    std::uniform_real_distribution<float> angle_dist(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> radius_dist(2.0f, 5.0f);

    int spawned = 0;
    for (int i = 0; i < count; ++i) {
        const float angle = angle_dist(s_rng);
        const float radius = radius_dist(s_rng);
        const glm::vec3 pos = around + glm::vec3(std::cos(angle) * radius, 1.0f, std::sin(angle) * radius);
        auto animal = std::make_unique<Passive_Animal>(kind,
        std::string("Summoned ") + passive_stats(kind).label + " #" + std::to_string(++s_summon_counter), pos);
        if (!animal->load_model()) break;
        passive_list(kind).push_back(std::move(animal));
        ++spawned;
    }
    return spawned;
}
int Mobs_Update::summon_hostile(Hostile_Kind kind, int count, const glm::vec3& around) {
    static int s_summon_counter = 0;
    static std::mt19937 s_rng(std::random_device{}());
    std::uniform_real_distribution<float> angle_dist(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> radius_dist(8.0f, 14.0f);

    int spawned = 0;
    for (int i = 0; i < count; ++i) {
        const float angle = angle_dist(s_rng);
        const float radius = radius_dist(s_rng);
        const glm::vec3 pos = around + glm::vec3(std::cos(angle) * radius, 1.0f, std::sin(angle) * radius);
        auto hostile = std::make_unique<Hostile_Mob>(kind,
        std::string("Summoned ") + hostile_stats(kind).label + " #" + std::to_string(++s_summon_counter), pos);
        if (!hostile->load_model()) break;
        m_hostiles.push_back(std::move(hostile));
        ++spawned;
    }
    return spawned;}

int Mobs_Update::summon_pigs(int count, const glm::vec3& around) {
    static int s_summon_counter = 0;
    static std::mt19937 s_rng(std::random_device{}());
    std::uniform_real_distribution<float> angle_dist(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> radius_dist(2.0f, 5.0f);

    int spawned = 0;
    for (int i = 0; i < count; ++i) {
        const float angle = angle_dist(s_rng);
        const float radius = radius_dist(s_rng);
        const glm::vec3 pos = around + glm::vec3(std::cos(angle) * radius, 1.0f, std::sin(angle) * radius);
        auto pig = std::make_unique<Pig>("Summoned #" + std::to_string(++s_summon_counter), pos);
        if (!pig->load_model("assets/mobs/pig.glb")) break;
        m_pigs.push_back(std::move(pig));
        ++spawned;
    }
    return spawned;
}
int Mobs_Update::summon_cows(int count, const glm::vec3& around) {
    return summon_passive(Passive_Kind::Cow, count, around);
}
int Mobs_Update::summon_sheep(int count, const glm::vec3& around) {
    return summon_passive(Passive_Kind::Sheep, count, around);
}

int Mobs_Update::summon_zombies(int count, const glm::vec3& around) {
    return summon_hostile(Hostile_Kind::Zombie, count, around);
}

int Mobs_Update::summon_skeletons(int count, const glm::vec3& around) {
    return summon_hostile(Hostile_Kind::Skeleton, count, around);
}

int Mobs_Update::summon_spiders(int count, const glm::vec3& around) {
    return summon_hostile(Hostile_Kind::Spider, count, around);
}

int Mobs_Update::summon_horses(int count, const glm::vec3& around) {
    static int s_summon_counter = 0;
    static std::mt19937 s_rng(std::random_device{}());
    std::uniform_real_distribution<float> angle_dist(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> radius_dist(3.0f, 6.0f);

    int spawned = 0;
    for (int i = 0; i < count; ++i) {
        const float angle = angle_dist(s_rng);
        const float radius = radius_dist(s_rng);
        const glm::vec3 pos = around + glm::vec3(std::cos(angle) * radius, 1.0f, std::sin(angle) * radius);
        auto horse = std::make_unique<Horse>("Summoned horse #" + std::to_string(++s_summon_counter), pos);
        if (!horse->load_model("assets/mobs/horse.glb")) break;
        m_horses.push_back(std::move(horse));
        ++spawned;
    }
    return spawned;
}

namespace {
    // Центр тела моба: от ступней на половину высоты ~ 0.5 блока.
    glm::vec3 body_center(const glm::vec3& feet) { return feet + glm::vec3(0.0f, 0.5f, 0.0f); }
}

int Mobs_Update::splash_potion(const glm::vec3& center, float radius, const Potion_Info& potion) {
    int affected = 0;
    const auto splash = [&](auto& mobs) {
        for (auto& mob : mobs) {
            if (mob->is_dying()) continue;
            if (glm::distance(body_center(mob->get_position()), center) > radius) continue;
            mob->effects().apply(potion.effect, potion.duration, potion.amplifier);
            ++affected;
        }
    };
    splash(m_pigs);
    splash(m_cows);
    splash(m_sheep);
    splash(m_horses);
    splash(m_hostiles);
    return affected;
}

bool Mobs_Update::mob_near(const glm::vec3& point, float radius) const {
    const auto near = [&](const auto& mobs) {
        for (const auto& mob : mobs) {
            if (mob->is_dying()) continue;
            if (glm::distance(body_center(mob->get_position()), point) <= radius) return true;
        }
        return false;
    };
    return near(m_pigs) || near(m_cows) || near(m_sheep) || near(m_horses) || near(m_hostiles);
}

int Mobs_Update::breed_all(const std::string& species) {
    int affected = 0;
    const auto love = [&](auto& animals) {
        for (auto& animal : animals) {
            if (!animal->is_dying() && animal->breeding().can_be_fed()) {
                animal->breeding().start_love();
                ++affected;
            }
        }
    };
    if (species == "pig") love(m_pigs);
    else if (species == "cow") love(m_cows);
    else if (species == "horse") love(m_horses);
    else if (species == "sheep") love(m_sheep);
    return affected;
}

// --- Спавн и размножение (внутренние) --------------------------------------------------------

void Mobs_Update::spawn_missing_pigs(const glm::vec3& player_position, Chunk_Manager& chunks) {
    static int s_pig_counter = 0; // для уникальных имён в логах ("Хрюша #3")
    static std::mt19937 s_rng(std::random_device{}());

    if (static_cast<int>(m_pigs.size()) >= Config::min_pig_count) return;
    if (m_pig_respawn_cooldown > 0.0f) return; // недавно кого-то убили — даём миру опустеть

    const int missing = Config::min_pig_count - static_cast<int>(m_pigs.size());
    for (int i = 0; i < missing; ++i) {
        std::optional<glm::vec3> spawn_position;
        for (int attempt = 0; attempt < k_spawn_attempts && !spawn_position; ++attempt) {
            spawn_position = find_ground_spawn(player_position, Config::pig_spawn_radius_min,
                                               Config::pig_spawn_radius_max, Spawn_Group::Animal, true,
                                               1.0f, chunks, s_rng);
        }
        if (!spawn_position) break; // подходящего места пока нет — следующий кадр

        auto pig = std::make_unique<Pig>("Хрюша #" + std::to_string(++s_pig_counter), *spawn_position);
        if (!pig->load_model("assets/mobs/pig.glb")) {
            LOG_ERROR("spawn_missing_pigs: failed to load assets/mobs/pig.glb");
            break; // модель битая/отсутствует — нет смысла пытаться ещё раз в этом же вызове
        }
        m_pigs.push_back(std::move(pig));
    }
}
void Mobs_Update::spawn_missing_passive(Passive_Kind kind, const glm::vec3& player_position, Chunk_Manager& chunks) {
    static std::mt19937 s_rng(std::random_device{}());
    const Passive_Stats& st = passive_stats(kind);
    auto& list = passive_list(kind);
    const int index = index_of(kind);

    if (static_cast<int>(list.size()) >= st.min_count) return;
    if (m_passive_respawn_cooldown[index] > 0.0f) return;

    const int missing = st.min_count - static_cast<int>(list.size());
    for (int i = 0; i < missing; ++i) {
        std::optional<glm::vec3> spawn_position;
        for (int attempt = 0; attempt < k_spawn_attempts && !spawn_position; ++attempt) {
            spawn_position = find_ground_spawn(player_position, st.spawn_radius_min, st.spawn_radius_max,
                                               Spawn_Group::Animal, true, 1.0f, chunks, s_rng);
        }
        if (!spawn_position) break;

        static int s_counter[2] = {0, 0};
        auto animal = std::make_unique<Passive_Animal>(kind,
        std::string(st.label) + " #" + std::to_string(++s_counter[index]), *spawn_position);
        if (!animal->load_model()) {
            LOG_ERROR(std::string("spawn_missing_passive: failed to load ") + st.model_path);
            m_passive_respawn_cooldown[index] = 30.0f; // не долбим диск битой моделью каждый кадр
            break;
        }
        list.push_back(std::move(animal));
    }
}

void Mobs_Update::spawn_missing_hostiles(Hostile_Kind kind, const glm::vec3& player_position, Chunk_Manager& chunks) {
    static std::mt19937 s_rng(std::random_device{}());
    const Hostile_Stats& st = hostile_stats(kind);
    const int index = index_of(kind);

    int count = 0;
    for (const auto& hostile : m_hostiles) {
        if (hostile->kind() == kind) ++count;
    }
    if (count >= st.min_count) return;
    if (m_hostile_respawn_cooldown[index] > 0.0f) return;

    // Одна попытка за тик: точка в кольце вокруг игрока, поверхность под ней и биом.
    const std::optional<glm::vec3> spawn_position = find_ground_spawn(
        player_position, st.spawn_radius_min, st.spawn_radius_max, Spawn_Group::Hostile, false, 1.0f, chunks, s_rng);
    if (!spawn_position) return; // незагруженный чанк или неподходящий биом — в другой раз

    static int s_counter[3] = {0, 0, 0};
    auto hostile = std::make_unique<Hostile_Mob>(kind,
        std::string(st.label) + " #" + std::to_string(++s_counter[index]), *spawn_position);
    if (!hostile->load_model()) {
        LOG_ERROR(std::string("spawn_missing_hostiles: failed to load ") + st.model_path);
        m_hostile_respawn_cooldown[index] = 30.0f;
        return;
    }
    m_hostiles.push_back(std::move(hostile));
}

void Mobs_Update::spawn_missing_horses(const glm::vec3& player_position, Chunk_Manager& chunks) {
    static int s_horse_counter = 0;
    static std::mt19937 s_rng(std::random_device{}());

    if (static_cast<int>(m_horses.size()) >= Config::min_horse_count) return;
    if (m_horse_respawn_cooldown > 0.0f) return; // недавно кого-то убили — даём миру опустеть

    // Одна попытка за кадр: лошадь встаёт на траву (два свободных блока над ней) в подходящем биоме.
    const std::optional<glm::vec3> spawn_position = find_ground_spawn(
        player_position, Config::horse_spawn_radius_min, Config::horse_spawn_radius_max,
        Spawn_Group::Horse, true, 1.02f, chunks, s_rng);
    if (!spawn_position) return;

    auto horse = std::make_unique<Horse>("Horse #" + std::to_string(++s_horse_counter), *spawn_position);
    if (!horse->load_model("assets/mobs/horse.glb")) {
        LOG_ERROR("spawn_missing_horses: failed to load assets/mobs/horse.glb");
        m_horse_respawn_cooldown = 30.0f; // не долбим диск загрузкой битой модели каждый кадр
        return;
    }
    m_horses.push_back(std::move(horse));
}

void Mobs_Update::update_breeding() {
    // --- Свиньи ---
    {
        std::vector<std::unique_ptr<Pig>> babies;
        for (auto [a, b] : find_breeding_pairs(m_pigs)) {
            if (!a->breeding().in_love() || !b->breeding().in_love()) continue; // уже участвовали в другой паре
            a->breeding().finish_breeding();
            b->breeding().finish_breeding();
            const glm::vec3 pos = (a->get_position() + b->get_position()) * 0.5f + glm::vec3(0.0f, 0.3f, 0.0f);
            auto baby = std::make_unique<Pig>("Piglet", pos);
            if (baby->load_model("assets/mobs/pig.glb")) {
                LOG_INFO("Pig born");
                baby->breeding().make_baby();
                babies.push_back(std::move(baby));
            }
        }
        for (auto& baby : babies) m_pigs.push_back(std::move(baby));
    }
    // --- Коровы и овцы: пары ищутся внутри одного вида ---
    for (Passive_Kind kind : {Passive_Kind::Cow, Passive_Kind::Sheep}) {
        const Passive_Stats& st = passive_stats(kind);
        auto& list = passive_list(kind);
        std::vector<std::unique_ptr<Passive_Animal>> babies;
        for (auto [a, b] : find_breeding_pairs(list)) {
            if (!a->breeding().in_love() || !b->breeding().in_love()) continue; // уже участвовали в другой паре
            a->breeding().finish_breeding();
            b->breeding().finish_breeding();
            const glm::vec3 pos = (a->get_position() + b->get_position()) * 0.5f + glm::vec3(0.0f, 0.3f, 0.0f);
            auto baby = std::make_unique<Passive_Animal>(kind, std::string(st.label) + " baby", pos);
            if (baby->load_model()) {
                LOG_INFO(std::string(st.label) + " born");
                baby->breeding().make_baby();
                babies.push_back(std::move(baby));
            }
        }
        for (auto& baby : babies) m_cows.push_back(std::move(baby));
    }
    // --- Лошади (жеребёнок запоминает, чем кормили родителей) ---
    {
        std::vector<std::unique_ptr<Horse>> babies;
        for (auto [a, b] : find_breeding_pairs(m_horses)) {
            if (!a->breeding().in_love() || !b->breeding().in_love()) continue; // уже участвовали в другой паре
            const Block_Types food_a = a->get_last_food();
            const Block_Types food_b = b->get_last_food();
            a->breeding().finish_breeding();
            b->breeding().finish_breeding();
            const glm::vec3 pos = (a->get_position() + b->get_position()) * 0.5f + glm::vec3(0.0f, 0.3f, 0.0f);
            auto baby = std::make_unique<Horse>("Foal", pos);
            if (baby->load_model("assets/mobs/horse.glb")) {
                LOG_INFO(std::string("Horse born; parents last food: ") + get_block_props(food_a).name + " / " +
                         get_block_props(food_b).name);
                baby->breeding().make_baby();
                baby->set_parent_foods(food_a, food_b);
                babies.push_back(std::move(baby));
            }
        }
        for (auto& baby : babies) m_horses.push_back(std::move(baby));
    }
}

// --- Отрисовка -------------------------------------------------------------------------------

void Mobs_Update::render(const Shader& shader, const glm::mat4& view, const glm::mat4& projection, float ambient) const {
    for (const auto& pig : m_pigs) {
        pig->render(shader, view, projection, ambient);
    }
    for (const auto& cow : m_cows) {
        cow->render(shader, view, projection, ambient);
    }
    for (const auto& sheep : m_sheep) {
        sheep->render(shader, view, projection, ambient);
    }
    for (const auto& horse : m_horses) {
        horse->render(shader, view, projection, ambient);
    }
    for (const auto& hostile : m_hostiles) {
        hostile->render(shader, view, projection, ambient);
    }
}
