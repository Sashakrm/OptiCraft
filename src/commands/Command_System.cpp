#include "commands/Command_System.h"

#include "core/game/Game.h"
#include "world/blocks/Block_Types.h"
#include "utils/Config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace {

// ---------------------------------------------------------------------------
//  Разбор строки
// ---------------------------------------------------------------------------
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::istringstream stream(input);
    std::string word;
    while (stream >> word) tokens.push_back(word);
    return tokens;
}

// Имя блока -> ключ для сравнения: нижний регистр, пробелы -> '_' ("Oak Log" == "oak_log").
std::string block_key(const std::string& name) {
    std::string key = to_lower(name);
    std::replace(key.begin(), key.end(), ' ', '_');
    return key;
}

// Ищет блок по имени ("oak_log", "Oak Log") или по числовому id. Air находится только
// при allow_air. Пустые имена — свободные слоты реестра, их пропускаем.
bool find_block(const std::string& text, bool allow_air, Block_Types& out) {
    const Block_Registry& registry = Block_Registry::get_instance();

    // Числовой id
    if (!text.empty() && std::all_of(text.begin(), text.end(),
                                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        const int id = std::atoi(text.c_str());
        if (id < 0 || id >= static_cast<int>(k_max_block_types)) return false;
        const Block_ID block_id = static_cast<Block_ID>(id);
        if (block_id == 0) { out = Block_Types::Air; return allow_air; }
        if (registry.get_properties(block_id).name.empty()) return false;
        out = static_cast<Block_Types>(block_id);
        return true;
    }

    const std::string wanted = block_key(text);
    for (size_t raw_id = 0; raw_id < Block_Registry::get_instance().id_capacity(); ++raw_id) {
        const Block_ID id = static_cast<Block_ID>(raw_id);
        const std::string& name = registry.get_properties(id).name;
        if (name.empty()) continue;
        if (block_key(name) != wanted) continue;
        if (id == 0 && !allow_air) return false;
        out = static_cast<Block_Types>(id);
        return true;
    }
    return false;
}

// '~' и '~<смещение>' — относительно текущей позиции (как в Minecraft).
bool parse_coord(const std::string& token, float base, float& out) {
    if (token.empty()) return false;
    try {
        size_t used = 0;
        if (token[0] == '~') {
            const float offset = (token.size() == 1) ? 0.0f : std::stof(token.substr(1), &used);
            if (token.size() > 1 && used != token.size() - 1) return false;
            out = base + offset;
        } else {
            out = std::stof(token, &used);
            if (used != token.size()) return false;
        }
        return std::isfinite(out);
    } catch (...) {
        return false;
    }
}

bool parse_int(const std::string& token, int& out) {
    try {
        size_t used = 0;
        out = std::stoi(token, &used);
        return used == token.size();
    } catch (...) {
        return false;
    }
}

// Склейка токенов с индекса from: "snowy", "mountains" -> "snowy mountains"
std::string join(const std::vector<std::string>& tokens, size_t from) {
    std::string out;
    for (size_t i = from; i < tokens.size(); ++i) {
        if (i > from) out += ' ';
        out += tokens[i];
    }
    return out;
}

std::string fmt_coord(float v) { return std::to_string(static_cast<int>(std::floor(v))); }

struct Command_Info {
    const char* name;
    const char* usage;
    const char* description;
};

// Шрифт HUD — только латиница, поэтому и справка на английском.
const Command_Info k_commands[] = {
    {"help",        "/help [command]",                              "List commands"},
    {"tp",          "/tp <x> <y> <z>",                              "Teleport (~ = relative)"},
    {"tpbiome",     "/tpbiome <biome>",                             "Teleport to nearest biome"},
    {"locatebiome", "/locatebiome <biome>",                         "Print coords of nearest biome"},
    {"time",        "/time set <day|noon|night|midnight|ticks>",    "Set time of day (also: add, query)"},
    {"tick",        "/tick speed <multiplier>",                    "Change simulation speed"},
    {"killall",     "/killall",                                     "Kill all mobs"},
    {"kill",        "/kill",                                        "Kill yourself (respawn)"},
    {"spawn",       "/spawn",                                       "Teleport to world spawn"},
    {"gamemode",    "/gamemode <creative|survival>",                "Change game mode"},
    {"fly",         "/fly",                                         "Toggle flying"},
    {"give",        "/give <block> [count]",                        "Give item (Survival)"},
    {"setblock",    "/setblock <x> <y> <z> <block>",                "Place a block (~ ok)"},
    {"summon",      "/summon <pig|horse|cow|sheep|zombie|skeleton|spider> [count]", "Spawn mobs near you"},
    {"breed",       "/breed <pig|cow|sheep|horse>",                 "Make all adult animals of a kind ready to breed"},
    {"clear",       "/clear",                                       "Remove dropped items"},
};

const Command_Info* find_info(const std::string& name) {
    for (const Command_Info& info : k_commands) {
        if (name == info.name) return &info;
    }
    return nullptr;
}

const char* const k_biome_hints[] = {
    "ocean", "shallow_water", "beach", "plains", "forest", "birch_forest", "taiga", "jungle",
    "savanna", "desert", "stone_desert", "steppe", "tundra", "ice_fields", "mountains",
    "high_mountains", "plateau", "slime", "solar", "sky", "underground", "deep_underground",
    "flooded_underground"
};

} // namespace

// ---------------------------------------------------------------------------
//  Исполнение
// ---------------------------------------------------------------------------
bool execute_command(Game& game, const std::string& input, std::string& reply) {
    std::vector<std::string> args = tokenize(input);
    if (args.empty()) { reply.clear(); return false; }

    // Имя команды и режимы/ключевые слова — без учёта регистра; имена блоков сравниваются
    // через block_key, координаты регистра не имеют.
    const std::string cmd = to_lower(args[0]);

    // --- /help -------------------------------------------------------------
    if (cmd == "help" || cmd == "?") {
        if (args.size() >= 2) {
            if (const Command_Info* info = find_info(to_lower(args[1]))) {
                reply = std::string(info->usage) + "\n" + info->description;
                return true;
            }
            reply = "Unknown command: " + args[1];
            return false;
        }
        reply = "Commands (T or / opens chat, TAB completes):";
        for (const Command_Info& info : k_commands) {
            reply += std::string("\n") + info.usage;
        }
        return true;
    }

    // --- /tp ---------------------------------------------------------------
    if (cmd == "tp" || cmd == "teleport") {
        if (args.size() != 4) { reply = "Usage: /tp <x> <y> <z>  (~ = relative)"; return false; }
        const glm::vec3 base = game.get_player_position();
        glm::vec3 target;
        if (!parse_coord(args[1], base.x, target.x) ||
            !parse_coord(args[2], base.y, target.y) ||
            !parse_coord(args[3], base.z, target.z)) {
            reply = "Invalid coordinates";
            return false;
        }
        if (target.y < static_cast<float>(Config::world_min_y) ||
            target.y > static_cast<float>(Config::world_max_y)) {
            reply = "Y is outside the world (" + std::to_string(Config::world_min_y) + ".." +
                    std::to_string(Config::world_max_y) + ")";
            return false;
        }
        game.teleport_player(target);
        reply = "Teleported to " + fmt_coord(target.x) + " " + fmt_coord(target.y) + " " + fmt_coord(target.z);
        return true;
    }

    // --- /tpbiome, /locatebiome ---------------------------------------------
    if (cmd == "tpbiome" || cmd == "locatebiome") {
        if (args.size() < 2) {
            reply = "Usage: /" + cmd + " <biome>  (e.g. mountains, birch_forest; see F3)";
            return false;
        }
        const std::string name = to_lower(join(args, 1));
        glm::vec3 pos;
        std::string found_name;
        if (!game.locate_biome(name, pos, found_name)) {
            reply = "Biome \"" + name + "\" not found within " +
                    std::to_string(Game::k_biome_search_radius) + " blocks";
            return false;
        }
        const std::string where = found_name + " at " + fmt_coord(pos.x) + " " + fmt_coord(pos.z);
        if (cmd == "tpbiome") {
            game.teleport_player(pos);
            reply = "Teleported to " + where;
        } else {
            reply = "Nearest " + where;
        }
        return true;
    }

    // --- /time -------------------------------------------------------------
    if (cmd == "time") {
        const std::string sub = args.size() >= 2 ? to_lower(args[1]) : "";

        if (sub == "query") {
            reply = "Time: " + std::to_string(game.get_day_time_ticks()) + " ticks";
            return true;
        }

        if ((sub == "set" || sub == "add") && args.size() >= 3) {
            const std::string value = to_lower(args[2]);
            // Тики Minecraft: 0 = рассвет, 6000 = полдень, 12000 = закат, 18000 = полночь.
            int ticks = 0;
            bool is_named = true;
            if      (value == "day")      ticks = 1000;
            else if (value == "noon")     ticks = 6000;
            else if (value == "sunset")   ticks = 12000;
            else if (value == "night")    ticks = 13000;
            else if (value == "midnight") ticks = 18000;
            else if (value == "sunrise" || value == "dawn") ticks = 0;
            else is_named = false;

            if (!is_named && !parse_int(value, ticks)) {
                reply = "Invalid time value: " + args[2];
                return false;
            }
            if (sub == "add") {
                if (is_named) { reply = "Usage: /time add <ticks>"; return false; }
                game.add_day_time_ticks(ticks);
            } else {
                if (ticks < 0) { reply = "Time must be >= 0"; return false; }
                game.set_day_time_ticks(ticks);
            }
            reply = "Time is now " + std::to_string(game.get_day_time_ticks()) + " ticks";
            return true;
        }

        reply = "Usage: /time set <day|noon|night|midnight|ticks>  |  /time add <ticks>  |  /time query";
        return false;
    }

    // --- /tick -------------------------------------------------------------
    if (cmd == "tick") {
        if (args.size() == 2 && to_lower(args[1]) == "speed") {
            std::ostringstream value;
            value << std::setprecision(4) << game.get_tick_speed();
            reply = "Tick speed: " + value.str() + "x";
            return true;
        }
        if (args.size() != 3 || to_lower(args[1]) != "speed") {
            reply = "Usage: /tick speed <multiplier>";
            return false;
        }
        float speed = 0.0f;
        if (!parse_coord(args[2], 0.0f, speed) || speed <= 0.0f || speed > 100.0f) {
            reply = "Speed must be a number greater than 0 and at most 100";
            return false;
        }
        game.set_tick_speed(speed);
        std::ostringstream value;
        value << std::setprecision(4) << speed;
        reply = "Tick speed set to " + value.str() + "x";
        return true;
    }

    // --- Мобы / игрок ----------------------------------------------------------
    if (cmd == "killall") {
        const int killed = game.kill_all_mobs();
        reply = "Killed " + std::to_string(killed) + (killed == 1 ? " mob" : " mobs");
        return true;
    }

    if (cmd == "kill") {
        game.kill_player();
        reply = "You died";
        return true;
    }

    if (cmd == "spawn") {
        game.goto_spawn();
        reply = "Teleported to spawn";
        return true;
    }

    if (cmd == "gamemode" || cmd == "gm") {
        if (args.size() != 2) { reply = "Usage: /gamemode <creative|survival|spectator>"; return false; }
        const std::string mode = to_lower(args[1]);
        Game_Mode wanted = Game_Mode::Creative;
        bool spectator = false;
        if (mode == "survival" || mode == "s" || mode == "0")      wanted = Game_Mode::Survival;
        else if (mode == "creative" || mode == "c" || mode == "1") wanted = Game_Mode::Creative;
        else if (mode == "spectator" || mode == "sp" || mode == "2") spectator = true;
        else { reply = "Unknown game mode: " + args[1]; return false; }

        game.apply_game_mode(wanted, spectator);
        reply = "Game mode: " + game.get_game_mode_name();
        return true;
    }

    if (cmd == "fly") {
        if (game.get_game_mode() == Game_Mode::Survival) { reply = "Flight is only available in Creative"; return false; }
        game.toggle_fly();
        reply = "Flying toggled";
        return true;
    }

    // --- /give <block> [count] -----------------------------------------------
    if (cmd == "give") {
        if (args.size() < 2 || args.size() > 3) { reply = "Usage: /give <block> [count]"; return false; }
        Block_Types type;
        if (!find_block(args[1], false, type)) { reply = "Unknown block: " + args[1]; return false; }

        int count = 1;
        if (args.size() == 3 && !parse_int(args[2], count)) { reply = "Invalid count: " + args[2]; return false; }
        count = std::clamp(count, 1, 2304); // 36 слотов * 64

        const int added = game.give_item(type, count);
        if (added <= 0) { reply = "Inventory is full"; return false; }
        reply = "Gave " + std::to_string(added) + " x " + get_block_props(type).name;
        if (added < count) reply += " (inventory full)";
        return true;
    }

    // --- /setblock x y z <block> ------------------------------------------------
    if (cmd == "setblock") {
        if (args.size() != 5) { reply = "Usage: /setblock <x> <y> <z> <block>"; return false; }
        const glm::vec3 base = game.get_player_position();
        glm::vec3 pos;
        if (!parse_coord(args[1], base.x, pos.x) ||
            !parse_coord(args[2], base.y, pos.y) ||
            !parse_coord(args[3], base.z, pos.z)) {
            reply = "Invalid coordinates";
            return false;
        }
        Block_Types type;
        if (!find_block(args[4], true, type)) { reply = "Unknown block: " + args[4]; return false; }

        const int x = static_cast<int>(std::floor(pos.x));
        const int y = static_cast<int>(std::floor(pos.y));
        const int z = static_cast<int>(std::floor(pos.z));
        if (!game.set_block_at(x, y, z, type)) {
            reply = "Can't place there (chunk not loaded or Y outside the world)";
            return false;
        }
        reply = "Placed " + std::string(type == Block_Types::Air ? "Air" : get_block_props(type).name) +
                " at " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z);
        return true;
    }

    // --- /summon <вид> [count] --------------------------------------------------------
    if (cmd == "summon") {
        std::string what = args.size() >= 2 ? to_lower(args[1]) : "pig";
        if (what.size() > 1 && what.back() == 's') what.pop_back(); // pigs -> pig, skeletons -> skeleton
        const char* usage = "Usage: /summon <pig|horse|cow|sheep|zombie|skeleton|spider> [count]";
        if (what != "pig" && what != "horse" && what != "cow" && what != "sheep" && what != "zombie" && what != "skeleton" && what != "spider") {
            reply = usage;
            return false;
        }
        int count = 1;
        if (args.size() >= 3 && !parse_int(args[2], count)) { reply = "Invalid count: " + args[2]; return false; }
        count = std::clamp(count, 1, 50);
        int spawned = 0;
        if (what == "pig") spawned = game.summon_pigs(count);
        else if (what == "horse") spawned = game.summon_horses(count);
        else if (what == "cow") spawned = game.summon_cows(count);
        else if (what == "sheep") spawned = game.summon_sheep(count);
        else if (what == "zombie") spawned = game.summon_zombies(count);
        else if (what == "spider") spawned = game.summon_spiders(count);
        else spawned = game.summon_skeletons(count);
        if (spawned <= 0) { reply = "Failed to summon " + what + " (model missing?)"; return false; }
        reply = "Summoned " + std::to_string(spawned) + " " + what + (spawned == 1 ? "" : "s");
        return true;
    }

    if (cmd == "breed") {
        const std::string what = args.size() >= 2 ? to_lower(args[1]) : "";
        if (what != "pig" && what != "cow" && what != "horse") { reply = "Usage: /breed <pig|cow|horse>"; return false; }
        reply = "Ready to breed: " + std::to_string(game.breed_all(what)) + " " + what + "(s)";
        return true;
    }

    if (cmd == "clear") {
        reply = "Removed " + std::to_string(game.clear_dropped_items()) + " dropped items";
        return true;
    }

    reply = "Unknown command: /" + args[0] + "  (try /help)";
    return false;
}

// ---------------------------------------------------------------------------
//  Tab-дополнение
// ---------------------------------------------------------------------------
std::vector<std::string> get_command_completions(const std::string& raw_input) {
    std::vector<std::string> result;

    const bool has_slash = !raw_input.empty() && raw_input[0] == '/';
    if (!has_slash) return result; // обычные сообщения не дополняем

    const std::string input = raw_input.substr(1);
    const bool ends_with_space = !input.empty() && input.back() == ' ';
    const std::vector<std::string> tokens = tokenize(input);

    // Список кандидатов на текущий (последний) токен + префикс перед ним.
    std::vector<std::string> candidates;
    std::string prefix_text = "/";       // всё, что уже введено до дополняемого слова
    std::string partial;                 // недописанное слово

    if (tokens.empty() || (tokens.size() == 1 && !ends_with_space)) {
        partial = tokens.empty() ? "" : tokens[0];
        for (const Command_Info& info : k_commands) candidates.push_back(info.name);
    } else {
        const std::string cmd = to_lower(tokens[0]);
        const size_t word_index = ends_with_space ? tokens.size() : tokens.size() - 1;
        partial = ends_with_space ? "" : tokens.back();
        for (size_t i = 0; i < word_index; ++i) prefix_text += tokens[i] + " ";

        if (cmd == "give" && word_index == 1) {
            const Block_Registry& registry = Block_Registry::get_instance();
            for (size_t raw_id = 1; raw_id < Block_Registry::get_instance().id_capacity(); ++raw_id) {
                const Block_ID id = static_cast<Block_ID>(raw_id);
                const std::string& name = registry.get_properties(id).name;
                if (!name.empty()) candidates.push_back(block_key(name));
            }
        } else if (cmd == "setblock" && word_index == 4) {
            const Block_Registry& registry = Block_Registry::get_instance();
            for (size_t raw_id = 0; raw_id < Block_Registry::get_instance().id_capacity(); ++raw_id) {
                const Block_ID id = static_cast<Block_ID>(raw_id);
                const std::string& name = registry.get_properties(id).name;
                if (!name.empty()) candidates.push_back(block_key(name));
            }
        } else if ((cmd == "tpbiome" || cmd == "locatebiome") && word_index == 1) {
            for (const char* biome : k_biome_hints) candidates.push_back(biome);
        } else if ((cmd == "gamemode" || cmd == "gm") && word_index == 1) {
            candidates = {"creative", "survival"};
        } else if (cmd == "time" && word_index == 1) {
            candidates = {"set", "add", "query"};
        } else if (cmd == "tick" && word_index == 1) {
            candidates = {"speed"};
        } else if (cmd == "time" && word_index == 2 && to_lower(tokens[1]) == "set") {
            candidates = {"day", "noon", "sunset", "night", "midnight", "sunrise"};
        } else if (cmd == "summon" && word_index == 1) {
            candidates = {"pig", "horse", "cow", "sheep", "zombie", "skeleton"};
        } else if (cmd == "breed" && word_index == 1) {
            candidates = {"pig", "horse", "cow", "sheep"};
        } else if (cmd == "help" && word_index == 1) {
            for (const Command_Info& info : k_commands) candidates.push_back(info.name);
        }
    }

    const std::string lowered = to_lower(partial);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    for (const std::string& candidate : candidates) {
        if (candidate.compare(0, lowered.size(), lowered) == 0) {
            result.push_back(prefix_text + candidate);
        }
    }
    return result;
}
