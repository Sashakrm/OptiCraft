#include "world/crafting/Smelting.h"

#include <algorithm>
#include <filesystem>
#include "utils/text/Json.h"
#include "utils/logging/Logger.h"

namespace {
    constexpr const char* k_smelting_path = "assets/smelting.json";

    const Json_Value& field_or_null(const Json_Value* v) {
        static const Json_Value k_null{};
        return v ? *v : k_null;
    }
}

Smelting_Registry& Smelting_Registry::get_instance() {
    static Smelting_Registry instance;
    return instance;
}

Smelting_Registry::Smelting_Registry() {
    if (std::filesystem::exists(k_smelting_path)) {
        load_from_file(k_smelting_path);
    } else {
        LOG_ERROR(std::string("Smelting_Registry: ") + k_smelting_path + " not found, furnaces will not smelt");
    }
}

const Smelting_Recipe* Smelting_Registry::find(Block_Types input) const {
    const auto it = m_recipes.find(static_cast<Block_ID>(input));
    return it == m_recipes.end() ? nullptr : &it->second;
}

float Smelting_Registry::get_fuel_time(Block_Types item) const {
    const auto it = m_fuel_seconds.find(static_cast<Block_ID>(item));
    return it == m_fuel_seconds.end() ? 0.0f : it->second;
}

void Smelting_Registry::load_from_file(const std::string& path) {
    Json_Value root;
    try {
        root = parse_json_file(path);
    } catch (const std::exception& ex) {
        LOG_ERROR("Smelting_Registry: failed to parse " + path + ": " + ex.what());
        return;
    }

    const Block_Registry& registry = Block_Registry::get_instance();
    // Air (id 0) здесь означает \"имя не найдено\": такую запись пропускаем с сообщением в лог.
    auto resolve = [&](const std::string& name) -> Block_Types {
        const Block_ID id = registry.get_block_id(name);
        if (id == 0) {
            LOG_ERROR("Smelting_Registry: unknown item \"" + name + "\" (entry skipped)");
        }
        return static_cast<Block_Types>(id);
    };

    if (const Json_Value* smelting = root.find("smelting"); smelting && smelting->is_array()) {
        for (const Json_Value& entry : smelting->array_value) {
            if (!entry.is_object()) continue;

            Smelting_Recipe recipe;
            recipe.input = resolve(json_read_string(field_or_null(entry.find("input"))));
            recipe.output = resolve(json_read_string(field_or_null(entry.find("output"))));
            if (recipe.input == Block_Types::Air || recipe.output == Block_Types::Air) continue;

            recipe.output_count = std::max(1, json_read_int(field_or_null(entry.find("count")), 1));
            recipe.time_seconds = std::max(0.1f, json_read_float(field_or_null(entry.find("time")), 10.0f));
            m_recipes[static_cast<Block_ID>(recipe.input)] = recipe;
        }
    }

    if (const Json_Value* fuels = root.find("fuels"); fuels && fuels->is_array()) {
        for (const Json_Value& entry : fuels->array_value) {
            if (!entry.is_object()) continue;

            const Block_Types item = resolve(json_read_string(field_or_null(entry.find("item"))));
            if (item == Block_Types::Air) continue;

            const float burn_time = json_read_float(field_or_null(entry.find("burn_time")), 0.0f);
            if (burn_time > 0.0f) m_fuel_seconds[static_cast<Block_ID>(item)] = burn_time;
        }
    }
}
