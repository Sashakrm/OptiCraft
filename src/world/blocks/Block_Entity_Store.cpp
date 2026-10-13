#include "world/blocks/Block_Entity_Store.h"

#include <fstream>
#include <sstream>
#include "world/blocks/Block_Types.h"
#include "utils/text/Json.h"
#include "utils/logging/Logger.h"

namespace {
    std::string escape_json_string(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (char c : value) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out;
    }

    void write_item_stack(std::ostringstream& out, size_t slot_index, const Item_Stack& stack) {
        // Пустые слоты не пишем вовсе — при загрузке всё, что не перечислено явно,
        // остаётся пустым по умолчанию (см. Block_Entity::make), так файл заметно короче.
        if (stack.is_empty()) return;
        const std::string& name = get_block_props(stack.type).name;
        out << "{\"slot\":" << slot_index
            << ",\"item\":\"" << escape_json_string(name) << "\""
            << ",\"count\":" << stack.count;
        if (stack.damage > 0) out << ",\"damage\":" << stack.damage;
        out << "},";
    }
}

namespace Block_Entity_Store {

bool load(const std::filesystem::path& path, Block_Entity_Map& out_entities) {
    out_entities.clear();
    if (!std::filesystem::exists(path)) {
        return false; // новый мир/пресет без единого контейнера — это не ошибка
    }

    Json_Value root;
    try {
        root = parse_json_file(path.string());
    } catch (const std::exception& ex) {
        LOG_ERROR("Block_Entity_Store::load: failed to parse " + path.string() + ": " + ex.what());
        return false;
    }

    const Json_Value* entities = root.find("entities");
    if (!entities || !entities->is_array()) {
        return false;
    }

    const Block_Registry& registry = Block_Registry::get_instance();

    for (const Json_Value& entry : entities->array_value) {
        if (!entry.is_object()) continue;

        const Json_Value* x = entry.find("x");
        const Json_Value* y = entry.find("y");
        const Json_Value* z = entry.find("z");
        const Json_Value* type_name = entry.find("type");
        if (!x || !y || !z || !type_name || !type_name->is_string()) continue;

        const Block_Entity_Type entity_type = block_entity_type_from_name(type_name->string_value);
        if (entity_type == Block_Entity_Type::None) continue;

        Block_Entity entity = Block_Entity::make(entity_type);
        entity.paired_dx = static_cast<int8_t>(json_read_int(entry.find("paired_dx") ? *entry.find("paired_dx") : Json_Value{}, 0));
        entity.paired_dz = static_cast<int8_t>(json_read_int(entry.find("paired_dz") ? *entry.find("paired_dz") : Json_Value{}, 0));

        entity.burn_time_left = json_read_float(entry.find("burn_left") ? *entry.find("burn_left") : Json_Value{}, 0.0f);
        entity.burn_time_total = json_read_float(entry.find("burn_total") ? *entry.find("burn_total") : Json_Value{}, 0.0f);
        entity.cook_progress = json_read_float(entry.find("cook") ? *entry.find("cook") : Json_Value{}, 0.0f);

        const Json_Value* slots = entry.find("slots");
        if (slots && slots->is_array()) {
            for (const Json_Value& slot_entry : slots->array_value) {
                if (!slot_entry.is_object()) continue;
                const Json_Value* slot_index_value = slot_entry.find("slot");
                const Json_Value* item_name_value = slot_entry.find("item");
                const Json_Value* count_value = slot_entry.find("count");
                if (!slot_index_value || !item_name_value || !item_name_value->is_string()) continue;

                const size_t slot_index = static_cast<size_t>(json_read_int(*slot_index_value, -1));
                if (slot_index >= entity.slots.size()) continue; // битые/устаревшие данные — молча пропускаем

                const Block_ID id = registry.get_block_id(item_name_value->string_value);
                if (id == 0) continue; // блок с таким именем больше не существует (например, правили blocks.json)

                Item_Stack stack;
                stack.type = static_cast<Block_Types>(id);
                stack.count = count_value ? json_read_int(*count_value, 0) : 0;
                if (const Json_Value* damage_value = slot_entry.find("damage")) {
                    stack.damage = std::max(0, json_read_int(*damage_value, 0));
                }
                entity.slots[slot_index] = stack;
            }
        }

        const glm::ivec3 world_pos{
            json_read_int(*x, 0), json_read_int(*y, 0), json_read_int(*z, 0)
        };
        out_entities[world_pos] = std::move(entity);
    }

    return true;
}

bool save(const std::filesystem::path& path, const Block_Entity_Map& entities) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ostringstream out;
    out << "{\"entities\":[";
    bool first = true;
    for (const auto& [world_pos, entity] : entities) {
        if (entity.type == Block_Entity_Type::None) continue;
        if (!first) out << ",";
        first = false;

        out << "{\"x\":" << world_pos.x << ",\"y\":" << world_pos.y << ",\"z\":" << world_pos.z
            << ",\"type\":\"" << block_entity_type_name(entity.type) << "\""
            << ",\"paired_dx\":" << static_cast<int>(entity.paired_dx)
            << ",\"paired_dz\":" << static_cast<int>(entity.paired_dz);
        if (entity.type == Block_Entity_Type::Furnace) {
            out << ",\"burn_left\":" << entity.burn_time_left
                << ",\"burn_total\":" << entity.burn_time_total
                << ",\"cook\":" << entity.cook_progress;
        }
        out
            << ",\"slots\":[";

        std::ostringstream slots_stream;
        for (size_t i = 0; i < entity.slots.size(); ++i) {
            write_item_stack(slots_stream, i, entity.slots[i]);
        }
        std::string slots_text = slots_stream.str();
        if (!slots_text.empty()) slots_text.pop_back(); // хвостовая запятая
        out << slots_text << "]}";
    }
    out << "]}";

    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        LOG_ERROR("Block_Entity_Store::save: failed to open " + path.string() + " for writing");
        return false;
    }
    file << out.str();
    return true;
}

} // namespace Block_Entity_Store
