//
// Created by noktemor on 28.03.2026.
//

#include "world/blocks/Block_Types.h"
#include "utils/logging/Logger.h"
#include "utils/text/Json.h"
#include "rendering/atlas/Atlas_Registry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {
    // Тонкие обёртки над общими json_* хелперами (Json.h) — оставлены под теми же
    // короткими именами, которыми пользуется остальной код этого файла ниже, чтобы
    // не трогать десятки вызовов read_int(...)/read_bool(...)/trim(...) по всему файлу.
    std::string trim(const std::string& value) { return json_trim(value); }
    std::string lower(std::string value) { return json_lower(std::move(value)); }
    bool read_bool(const Json_Value& value, bool fallback) { return json_read_bool(value, fallback); }
    int read_int(const Json_Value& value, int fallback) { return json_read_int(value, fallback); }
    float read_float(const Json_Value& value, float fallback) { return json_read_float(value, fallback); }

    // value-or-null для Json_Value::find (который отдаёт nullptr, когда поля нет).
    const Json_Value& field_or_null(const Json_Value* v) {
        static const Json_Value k_null{};
        return v ? *v : k_null;
    }

    // Резолвит спрайт по имени в конкретном атласе в готовый UV-прямоугольник.
    // atlas_name пустой/не найден или sprite_name отсутствует — лог уже пишет сам
    // Atlas_Registry::get_atlas/Texture_Atlas::get_uv_coords, здесь просто отдаём
    // (0,0,0,0) как безопасный fallback ("дырка" в текстуре, а не краш).
    glm::vec4 resolve_sprite_uv(const std::string& atlas_name, const std::string& sprite_name) {
        if (atlas_name.empty() || sprite_name.empty()) {
            return glm::vec4(0.0f);
        }
        const Texture_Atlas* atlas = Atlas_Registry::get_instance().get_atlas(atlas_name);
        if (!atlas) {
            return glm::vec4(0.0f);
        }
        return atlas->get_uv_coords(sprite_name);
    }

    // Читает набор текстур граней из объекта {"top","side","bottom","front","north","south",
    // "east","west"}. fallback — уже готовый набор, из которого берутся все отсутствующие ключи
    // (для active_texture); для обычного набора fallback = nullptr.
    Block_Face_Textures read_face_textures(const Json_Value& object, const std::string& atlas_name,
                                           const Block_Face_Textures* fallback) {
        Block_Face_Textures t = fallback ? *fallback : Block_Face_Textures{};
        auto read = [&](const char* key, glm::vec4& out) -> bool {
            if (const auto* v = object.find(key); v && v->is_string()) {
                out = resolve_sprite_uv(atlas_name, v->string_value);
                return true;
            }
            return false;
        };
        read("top", t.top);
        read("side", t.side);
        read("bottom", t.bottom);
        if (read("front", t.front)) t.has_front = true;

        // Переопределения "по сторонам света". Сторона света — в мировых осях:
        // north = -Z, south = +Z, east = +X, west = -X (поворот Facing-блоков их не двигает).
        struct { const char* key; int face; } named[] = {
            {"east", Block_Face::pos_x}, {"west", Block_Face::neg_x},
            {"south", Block_Face::pos_z}, {"north", Block_Face::neg_z}
        };
        for (const auto& n : named) {
            if (read(n.key, t.override_uv[n.face])) t.has_override[n.face] = true;
        }
        return t;
    }

    Block_Mesh_Type parse_mesh_type(const Json_Value& value, Block_Mesh_Type fallback) {
        if (value.is_number()) {
            return static_cast<Block_Mesh_Type>(static_cast<int>(std::lround(value.number_value)));
        }
        if (value.is_string()) {
            const std::string type = lower(trim(value.string_value));
            if (type == "solid") return Block_Mesh_Type::Solid;
            if (type == "flora") return Block_Mesh_Type::Flora;
            if (type == "liquid") return Block_Mesh_Type::Liquid;
        }
        return fallback;
    }

    Block_Mesh_Style parse_mesh_style(const Json_Value& value, Block_Mesh_Style fallback) {
        if (value.is_number()) {
            return static_cast<Block_Mesh_Style>(static_cast<int>(std::lround(value.number_value)));
        }
        if (value.is_string()) {
            const std::string type = lower(trim(value.string_value));
            if (type == "block") return Block_Mesh_Style::Block;
            if (type == "xstyle") return Block_Mesh_Style::XStyle;
        }
        return fallback;
    }

    Block_State parse_block_state(const Json_Value& value, Block_State fallback) {
        if (value.is_number()) {
            return static_cast<Block_State>(static_cast<int>(std::lround(value.number_value)));
        }
        if (value.is_string()) {
            const std::string type = lower(trim(value.string_value));
            if (type == "solid") return Block_State::Solid;
            if (type == "liquid") return Block_State::Liquid;
            if (type == "gas") return Block_State::Gas;
        }
        return fallback;
    }

    std::string resolve_blocks_path() {
        const std::array<std::string, 3> prefixes = {
            "assets/blocks.json",
            "../assets/blocks.json",
            "../../assets/blocks.json"
        };

        for (const auto& path : prefixes) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }

        return prefixes.front();
    }
}

Block_Registry::Block_Registry() {
    load_defaults();
    load_from_file(resolve_blocks_path());
    // Строим плоские таблицы ПОСЛЕ load_from_file, а не в load_defaults — файл может
    // переопределить is_transparent/light_emission для блоков из дефолтного набора
    // (или добавить новые id), так что снимок нужно делать только когда m_properties
    // уже окончательный. См. комментарий у объявления полей в Block_Types.h.
    build_fast_tables();
}

void Block_Registry::build_fast_tables() {
    for (size_t id = 0; id < k_max_block_types; ++id) {
        m_transparent_flat[id] = m_properties[id].is_transparent;
        // light_emission в Block_Properties — int (0..max_light_level), а таблица — uint8_t,
        // чтобы обе плоские таблицы весили ровно 256 байт и лежали в кэше одинаково дёшево;
        // диапазон освещения никогда не подойдёт к 255, так что сужение безопасно.
        m_light_emission_flat[id] = static_cast<uint8_t>(m_properties[id].light_emission);
    }
}

Block_Registry& Block_Registry::get_instance() {
    static Block_Registry instance;
    return instance;
}

const Block_Properties& Block_Registry::get_properties(Block_Types type) const {
    return get_properties(static_cast<Block_ID>(type));
}

const Block_Properties& Block_Registry::get_properties(Block_ID id) const {
    if (id >= static_cast<Block_ID>(m_properties.size())) {
        return m_properties[static_cast<size_t>(Block_Types::Air)];
    }

    return m_properties[id];
}

Block_ID Block_Registry::get_block_id(const std::string& name) const {
    for (Block_ID id = 0; id < m_properties.size(); ++id) {
        if (m_properties[id].name == name) {
            return id;
        }
    }

    return static_cast<Block_ID>(Block_Types::Air);
}

void Block_Registry::load_defaults() {
    // Имя намеренно пустое: это маркер "слот не занят" для всего проекта (Inventory и т.п.
    // пропускают блоки с пустым именем). Раньше здесь стояло "Unknown" — из-за этого
    // зарезервированный id 8 не пропускался и попадал в хотбар как выбираемый блок с
    // невалидными индексами текстуры (-1). Пустая строка чинит это по всей цепочке разом.
    const Block_Properties unassigned = {
        "", false, true, false, false, 0.0f,
        Block_Mesh_Type::Solid, Block_Mesh_Style::Block, Block_State::Gas,
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
    };
    m_properties.fill(unassigned);

    set_properties(static_cast<Block_ID>(Block_Types::Air), {
        "Air", false, true, false, false, 0.0f,
        Block_Mesh_Type::Solid, Block_Mesh_Style::Block, Block_State::Gas,
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
    });

    set_properties(static_cast<Block_ID>(Block_Types::Grass), {
        "Grass", true, false, true, false, 1.0f,
        Block_Mesh_Type::Solid, Block_Mesh_Style::Block, Block_State::Solid,
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
    });

    set_properties(static_cast<Block_ID>(Block_Types::Dirt), {
        "Dirt", true, false, true, false, 1.0f,
        Block_Mesh_Type::Solid, Block_Mesh_Style::Block, Block_State::Solid,
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
    });

    set_properties(static_cast<Block_ID>(Block_Types::Stone), {
        "Stone", true, false, true, false, 2.0f,
        Block_Mesh_Type::Solid, Block_Mesh_Style::Block, Block_State::Solid,
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
    });

    set_properties(static_cast<Block_ID>(Block_Types::Sand), {
        "Sand", true, false, true, false, 1.0f,
        Block_Mesh_Type::Solid, Block_Mesh_Style::Block, Block_State::Solid,
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
    });

    set_properties(static_cast<Block_ID>(Block_Types::Oak_Wood), {
        "Oak Wood", true, false, true, false, 2.0f,
        Block_Mesh_Type::Solid, Block_Mesh_Style::Block, Block_State::Solid,
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
    });

    set_properties(static_cast<Block_ID>(Block_Types::Oak_Leaf), {
        "Blocky", true, true, false, false, 0.2f,
        Block_Mesh_Type::Flora, Block_Mesh_Style::Block, Block_State::Solid,
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
    });

    set_properties(static_cast<Block_ID>(Block_Types::Water), {
        "Water", false, true, false, false, 100.0f,
        Block_Mesh_Type::Liquid, Block_Mesh_Style::Block, Block_State::Liquid,
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
    });

    set_properties(static_cast<Block_ID>(Block_Types::Rose), {
        "Rose", false, true, false, false, 0.0f,
        Block_Mesh_Type::Flora, Block_Mesh_Style::XStyle, Block_State::Solid,
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
    });

    set_properties(static_cast<Block_ID>(Block_Types::Tall_Grass), {
        "Tall Grass", false, true, false, false, 0.0f,
        Block_Mesh_Type::Flora, Block_Mesh_Style::XStyle, Block_State::Solid,
        glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
    });
}

void Block_Registry::load_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    try {
        Json_Parser parser(buffer.str());
        Json_Value root = parser.parse();
        if (!root.is_object()) {
            return;
        }

        if (const auto* reserved = root.find("reserved_block_ids"); reserved && reserved->is_array()) {
            for (const auto& entry : reserved->array_value) {
                if (!entry.is_number()) {
                    continue;
                }
                const int reserved_id = read_int(entry, -1);
                if (reserved_id >= 0 && reserved_id < static_cast<int>(m_reserved.size())) {
                    m_reserved[static_cast<size_t>(reserved_id)] = true;
                }
            }
        }

        const Json_Value* blocks = root.find("blocks");
        if (!blocks || !blocks->is_array()) {
            return;
        }

        // Имя блока-дропа может ссылаться на блок, описанный в файле ПОЗЖЕ текущего, поэтому
        // id дропа резолвится вторым проходом (после цикла), а здесь только запоминается имя.
        std::vector<std::pair<Block_ID, std::string>> pending_drops;

        for (const auto& entry : blocks->array_value) {
            if (!entry.is_object()) {
                continue;
            }

            Block_Properties properties = {
                "", true, false, true, false, 1.0f,
                Block_Mesh_Type::Solid, Block_Mesh_Style::Block, Block_State::Solid,
                glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f)
            };

            Block_ID id = static_cast<Block_ID>(Block_Types::Air);
            bool has_opaque = false;
            bool has_transparent = false;

            if (const auto* v = entry.find("id")) {
                id = static_cast<Block_ID>(read_int(*v, id));
            }
            if (const auto* v = entry.find("name"); v && v->is_string()) {
                properties.name = trim(v->string_value);
            }
            if (const auto* v = entry.find("solid")) {
                properties.is_solid = read_bool(*v, properties.is_solid);
            }
            if (const auto* v = entry.find("transparent")) {
                properties.is_transparent = read_bool(*v, properties.is_transparent);
                has_transparent = true;
            }
            if (const auto* v = entry.find("opaque")) {
                properties.is_opaque = read_bool(*v, properties.is_opaque);
                has_opaque = true;
            }
            if (const auto* v = entry.find("can_update")) {
                properties.can_update = read_bool(*v, properties.can_update);
            }
            if (const auto* v = entry.find("hardness")) {
                properties.hardness = read_float(*v, properties.hardness);
            }
            if (const auto* v = entry.find("light_emission")) {
                properties.light_emission = read_int(*v, properties.light_emission);
            }
            if (const auto* v = entry.find("item")) {
                properties.is_item = read_bool(*v, properties.is_item);
            }
            if (const auto* v = entry.find("food")) {
                properties.food_points = std::max(0, read_int(*v, properties.food_points));
            }
            if (const auto* v = entry.find("saturation")) {
                properties.food_saturation = std::max(0.0f, read_float(*v, properties.food_saturation));
            }
            if (const auto* v = entry.find("durability")) {
                properties.max_durability = std::max(0, read_int(*v, 0));
            }
            if (const auto* v = entry.find("tool_tier")) {
                properties.tool_tier = std::max(0, read_int(*v, 0));
            }
            if (const auto* v = entry.find("tool_speed")) {
                properties.tool_speed = std::max(1.0f, read_float(*v, 1.0f));
            }
            if (const auto* v = entry.find("attack_damage")) {
                properties.attack_damage = std::max(0.0f, read_float(*v, 0.0f));
            }
            if (const auto* v = entry.find("harvest_tier")) {
                properties.harvest_tier = std::max(0, read_int(*v, 0));
            }
            if (const auto* v = entry.find("creative")) {
                properties.show_in_creative = read_bool(*v, true);
            }
            auto parse_tool = [&](const char* key) {
                Tool_Type result = Tool_Type::None;
                if (const auto* t = entry.find(key); t && t->is_string()) {
                    const std::string n = lower(trim(t->string_value));
                    if (n == "pickaxe") result = Tool_Type::Pickaxe;
                    else if (n == "axe") result = Tool_Type::Axe;
                    else if (n == "shovel") result = Tool_Type::Shovel;
                    else if (n == "sword") result = Tool_Type::Sword;
                    else if (n == "hoe") result = Tool_Type::Hoe;
                }
                return result;
            };
            properties.tool_type = parse_tool("tool");
            properties.effective_tool = parse_tool("effective_tool");
            if (const auto* v = entry.find("break_time")) {
                properties.break_time = read_float(*v, properties.break_time);
            }
            if (const auto* v = entry.find("drop_count")) {
                properties.drop_count = std::max(0, read_int(*v, properties.drop_count));
            }
            if (const auto* v = entry.find("drop_chance")) {
                properties.drop_chance = std::clamp(read_float(*v, properties.drop_chance), 0.0f, 1.0f);
            }
            if (const auto* v = entry.find("drop"); v && v->is_string()) {
                // "self" (по умолчанию) — сам блок, "none" — ничего, иначе имя другого блока/предмета.
                const std::string drop_name = trim(v->string_value);
                if (drop_name == "none") {
                    properties.drop_id = 0;
                } else if (!drop_name.empty() && drop_name != "self") {
                    pending_drops.emplace_back(id, drop_name);
                }
            }
            if (const auto* v = entry.find("mesh_type")) {
                properties.mesh_type = parse_mesh_type(*v, properties.mesh_type);
            }
            if (const auto* v = entry.find("mesh_style")) {
                properties.mesh_style = parse_mesh_style(*v, properties.mesh_style);
            }
            if (const auto* v = entry.find("state")) {
                properties.state = parse_block_state(*v, properties.state);
            }

            if (const auto* v = entry.find("orientation"); v && v->is_string()) {
                const std::string o = lower(trim(v->string_value));
                if (o == "axis") properties.orientation = Block_Orientation::Axis;
                else if (o == "facing") properties.orientation = Block_Orientation::Facing;
                else if (o != "none") LOG_WARN("Block_Registry: unknown orientation '" + o + "' for '" + properties.name + "'");
            }

            if (const auto* texture = entry.find("texture"); texture && texture->is_object()) {
                // "atlas" — необязательное поле, по умолчанию "blocks_main" (единственный
                // атлас, который реально биндится при рендере чанков — см. предупреждение
                // в Renderer.h про один bound-атлас на draw call). Другие атласы (UI,
                // 3D-иконки блоков для инвентаря и т.п.) регистрируются через тот же
                // Atlas_Registry, но для мировых блоков сейчас имеет смысл только один.
                const std::string atlas_name = texture->find("atlas")
                    ? json_read_string(field_or_null(texture->find("atlas")), "blocks_main")
                    : "blocks_main";

                if (const auto* top = texture->find("top"); top && top->is_string()) {
                    properties.uv_top = resolve_sprite_uv(atlas_name, top->string_value);
                }
                if (const auto* side = texture->find("side"); side && side->is_string()) {
                    properties.uv_side = resolve_sprite_uv(atlas_name, side->string_value);
                }
                if (const auto* bottom = texture->find("bottom"); bottom && bottom->is_string()) {
                    properties.uv_bottom = resolve_sprite_uv(atlas_name, bottom->string_value);
                }
                properties.textures = read_face_textures(*texture, atlas_name, nullptr);

                // "active_texture" — набор для активного состояния (горящая печь). Любой ключ,
                // которого там нет, берётся из обычного набора, поэтому достаточно перечислить
                // только то, что меняется.
                if (const auto* active = entry.find("active_texture"); active && active->is_object()) {
                    properties.has_active_state = true;
                    properties.active_textures = read_face_textures(*active, atlas_name, &properties.textures);
                } else {
                    properties.active_textures = properties.textures;
                }
                properties.uv_icon = properties.textures.has_front ? properties.textures.front
                                                                   : properties.textures.top;
            }
            properties.uses_meta = properties.orientation != Block_Orientation::None ||
                                   properties.has_active_state;

            if (!properties.name.empty()) {
                if (is_reserved(id)) {
                    LOG_ERROR("Block_Registry: block '" + properties.name + "' targets reserved id " +
                              std::to_string(id) + ", skipped");
                } else {
                    if (has_opaque) {
                        properties.is_transparent = !properties.is_opaque;
                    } else if (has_transparent) {
                        properties.is_opaque = !properties.is_transparent;
                    }
                    set_properties(id, properties);
                }
            }
        }

        for (const auto& [block_id, drop_name] : pending_drops) {
            if (block_id >= m_properties.size() || m_properties[block_id].name.empty()) continue;
            const Block_ID drop_block = get_block_id(drop_name);
            if (drop_block == static_cast<Block_ID>(Block_Types::Air)) {
                LOG_ERROR("Block_Registry: block '" + m_properties[block_id].name +
                          "' has unknown drop '" + drop_name + "', it will drop itself");
                continue;
            }
            m_properties[block_id].drop_id = static_cast<int>(drop_block);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("Block_Registry: failed to parse blocks JSON from ") + path + " (" + e.what() + ")");
    }
}

void Block_Registry::set_properties(Block_ID id, const Block_Properties& properties) {
    if (id >= m_properties.size()) {
        return;
    }

    if (m_properties[id].name.empty() && !properties.name.empty()) {
        ++m_registered_block_count;
    } else if (!m_properties[id].name.empty() && properties.name.empty()) {
        --m_registered_block_count;
    }

    m_properties[id] = properties;
}

bool Block_Registry::is_reserved(Block_ID id) const {
    return id < m_reserved.size() && m_reserved[id];
}

const glm::vec4& Block_Properties::resolve_face_uv(int face, uint8_t meta, bool& rotate_quarter) const {
    rotate_quarter = false;
    const Block_Face_Textures& t = (has_active_state && (meta & Block_Meta::active_bit))
                                       ? active_textures : textures;
    const uint8_t direction = meta & Block_Meta::direction_mask;

    if (orientation == Block_Orientation::Axis) {
        // Торцы — на гранях вдоль оси, кора — на остальных.
        const bool along_x = direction == Block_Meta::axis_x;
        const bool along_z = direction == Block_Meta::axis_z;
        const bool is_end = along_x ? (face == Block_Face::pos_x || face == Block_Face::neg_x)
                          : along_z ? (face == Block_Face::pos_z || face == Block_Face::neg_z)
                                    : (face == Block_Face::pos_y || face == Block_Face::neg_y);
        if (is_end) return t.top;
        // Полосы коры в спрайте идут вертикально; у лежащего бревна они должны идти вдоль ствола.
        if (along_x) rotate_quarter = (face != Block_Face::pos_x && face != Block_Face::neg_x);
        else if (along_z) rotate_quarter = (face == Block_Face::pos_x || face == Block_Face::neg_x);
        return t.side;
    }

    if (t.has_override[face]) return t.override_uv[face];

    if (orientation == Block_Orientation::Facing && t.has_front) {
        static constexpr int k_front_face[4] = {Block_Face::neg_z, Block_Face::pos_x,
                                                Block_Face::pos_z, Block_Face::neg_x};
        if (face == k_front_face[direction]) return t.front;
    }

    if (face == Block_Face::pos_y) return t.top;
    if (face == Block_Face::neg_y) return t.bottom;
    return t.side;
}
