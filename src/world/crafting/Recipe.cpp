#include "world/crafting/Recipe.h"

#include <algorithm>
#include <filesystem>
#include "utils/text/Json.h"
#include "utils/logging/Logger.h"

namespace {
    // Путь ищем от текущей рабочей директории, как и остальные assets/* в этом
    // движке (см. Block_Registry::load_from_file) — сборка копирует assets/ рядом
    // с исполняемым файлом (см. CMakeLists.txt: copy_directory).
    constexpr const char* k_recipe_path = "assets/recipe.json";
}

Recipe_Registry& Recipe_Registry::get_instance() {
    static Recipe_Registry instance;
    return instance;
}

Recipe_Registry::Recipe_Registry() {
    if (std::filesystem::exists(k_recipe_path)) {
        load_from_file(k_recipe_path);
    } else {
        LOG_ERROR(std::string("Recipe_Registry: ") + k_recipe_path + " not found, crafting table will be empty");
    }
}

void Recipe_Registry::load_from_file(const std::string& path) {
    Json_Value root;
    try {
        root = parse_json_file(path);
    } catch (const std::exception& ex) {
        LOG_ERROR("Recipe_Registry: failed to parse " + path + ": " + ex.what());
        return;
    }

    const Json_Value* recipes = root.find("recipes");
    if (!recipes || !recipes->is_array()) {
        LOG_ERROR("Recipe_Registry: " + path + " has no \"recipes\" array");
        return;
    }

    const Block_Registry& registry = Block_Registry::get_instance();
    auto resolve_item = [&](const std::string& name) -> Block_Types {
        const Block_ID id = registry.get_block_id(name);
        if (id == 0 && name != "Air") {
            LOG_ERROR("Recipe_Registry: unknown item \"" + name + "\" (recipe skipped)");
        }
        return static_cast<Block_Types>(id);
    };

    // Теги: имя -> список предметов. Ссылаются на них как "#имя".
    std::vector<std::string> tag_names;
    if (const Json_Value* tags = root.find("tags"); tags && tags->is_object()) {
        for (const auto& [tag_name, list] : tags->object_value) {
            if (!list.is_array()) continue;
            std::vector<Block_Types> members;
            for (const Json_Value& item : list.array_value) {
                if (!item.is_string()) continue;
                const Block_ID id = registry.get_block_id(item.string_value);
                if (id != 0) members.push_back(static_cast<Block_Types>(id));
            }
            tag_names.push_back(tag_name);
            m_tags.push_back(std::move(members));
        }
    }
    auto find_tag = [&](const std::string& reference) -> int {
        if (reference.size() < 2 || reference[0] != '#') return -1;
        const std::string name = reference.substr(1);
        for (size_t i = 0; i < tag_names.size(); ++i) if (tag_names[i] == name) return static_cast<int>(i);
        LOG_ERROR("Recipe_Registry: unknown tag \"" + name + "\"");
        return -1;
    };

    for (const Json_Value& entry : recipes->array_value) {
        if (!entry.is_object()) continue;

        Recipe recipe;
        recipe.name = json_read_string(entry.find("name") ? *entry.find("name") : Json_Value{}, "");
        const std::string type = json_read_string(entry.find("type") ? *entry.find("type") : Json_Value{}, "shapeless");

        const Json_Value* output = entry.find("output");
        if (!output || !output->is_object()) {
            LOG_ERROR("Recipe_Registry: recipe \"" + recipe.name + "\" has no output, skipped");
            continue;
        }
        const std::string output_name = json_read_string(output->find("item") ? *output->find("item") : Json_Value{}, "");
        recipe.output_type = resolve_item(output_name);
        recipe.output_count = std::max(1, json_read_int(output->find("count") ? *output->find("count") : Json_Value{}, 1));
        if (recipe.output_type == Block_Types::Air) continue; // неизвестный/пустой выход — рецепт бесполезен

        bool valid = true;

        if (type == "shaped") {
            recipe.shapeless = false;
            const Json_Value* pattern = entry.find("pattern");
            const Json_Value* key = entry.find("key");
            if (!pattern || !pattern->is_array() || !key || !key->is_object()) {
                LOG_ERROR("Recipe_Registry: shaped recipe \"" + recipe.name + "\" missing pattern/key, skipped");
                continue;
            }

            const int height = static_cast<int>(pattern->array_value.size());
            int width = 0;
            for (const Json_Value& row : pattern->array_value) {
                if (row.is_string()) width = std::max(width, static_cast<int>(row.string_value.size()));
            }
            if (height < 1 || height > 3 || width < 1 || width > 3) {
                LOG_ERROR("Recipe_Registry: shaped recipe \"" + recipe.name + "\" pattern must fit in 3x3, skipped");
                continue;
            }

            recipe.shaped_width = width;
            recipe.shaped_height = height;
            for (int r = 0; r < height; ++r) {
                const std::string& row = pattern->array_value[static_cast<size_t>(r)].string_value;
                for (int c = 0; c < width; ++c) {
                    const char symbol = (c < static_cast<int>(row.size())) ? row[static_cast<size_t>(c)] : ' ';
                    Block_Types cell_type = Block_Types::Air;
                    int cell_tag = -1;
                    if (symbol != ' ') {
                        const std::string symbol_str(1, symbol);
                        const Json_Value* mapped = key->find(symbol_str);
                        if (!mapped || !mapped->is_string()) {
                            LOG_ERROR("Recipe_Registry: shaped recipe \"" + recipe.name +
                                      "\" key missing for '" + symbol_str + "', skipped");
                            valid = false;
                            break;
                        }
                        if (!mapped->string_value.empty() && mapped->string_value[0] == '#') {
                            cell_tag = find_tag(mapped->string_value);
                            if (cell_tag < 0) { valid = false; break; }
                        } else {
                            cell_type = resolve_item(mapped->string_value);
                            if (cell_type == Block_Types::Air) { valid = false; break; }
                        }
                    }
                    recipe.shaped_grid[static_cast<size_t>(r * width + c)] = cell_type;
                    recipe.shaped_tags[static_cast<size_t>(r * width + c)] = cell_tag;
                }
                if (!valid) break;
            }
        } else {
            recipe.shapeless = true;
            const Json_Value* ingredients = entry.find("ingredients");
            if (!ingredients || !ingredients->is_array() || ingredients->array_value.empty()) {
                LOG_ERROR("Recipe_Registry: shapeless recipe \"" + recipe.name + "\" has no ingredients, skipped");
                continue;
            }
            for (const Json_Value& ingredient : ingredients->array_value) {
                if (!ingredient.is_object()) continue;
                const std::string item_name = json_read_string(
                    ingredient.find("item") ? *ingredient.find("item") : Json_Value{}, "");
                const int count = std::max(1, json_read_int(
                    ingredient.find("count") ? *ingredient.find("count") : Json_Value{}, 1));
                if (!item_name.empty() && item_name[0] == '#') {
                    const int tag = find_tag(item_name);
                    if (tag < 0) { valid = false; break; }
                    recipe.tag_ingredients.emplace_back(tag, count);
                    continue;
                }
                const Block_Types item_type = resolve_item(item_name);
                if (item_type == Block_Types::Air) { valid = false; break; }
                recipe.ingredients.emplace_back(item_type, count);
            }
        }

        if (valid) {
            m_recipes.push_back(std::move(recipe));
        }
    }

    LOG_INFO("Recipe_Registry: loaded " + std::to_string(m_recipes.size()) + " recipe(s) from " + path);
}

bool Recipe_Registry::in_tag(int tag, Block_Types type) const {
    if (tag < 0 || tag >= static_cast<int>(m_tags.size())) return false;
    const auto& members = m_tags[static_cast<size_t>(tag)];
    return std::find(members.begin(), members.end(), type) != members.end();
}

bool Recipe_Registry::match_shaped(const Recipe& recipe, const std::array<Item_Stack, 9>& grid) const {
    int min_r = 3, max_r = -1, min_c = 3, max_c = -1;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if (!grid[static_cast<size_t>(r * 3 + c)].is_empty()) {
                min_r = std::min(min_r, r);
                max_r = std::max(max_r, r);
                min_c = std::min(min_c, c);
                max_c = std::max(max_c, c);
            }
        }
    }
    if (max_r < 0) return false; // пустая сетка

    const int height = max_r - min_r + 1;
    const int width = max_c - min_c + 1;
    if (height != recipe.shaped_height || width != recipe.shaped_width) return false;

    for (int r = 0; r < height; ++r) {
        for (int c = 0; c < width; ++c) {
            const size_t cell = static_cast<size_t>(r * recipe.shaped_width + c);
            const Block_Types expected = recipe.shaped_grid[cell];
            const Item_Stack& actual = grid[static_cast<size_t>((min_r + r) * 3 + (min_c + c))];
            const Block_Types actual_type = actual.is_empty() ? Block_Types::Air : actual.type;
            if (recipe.shaped_tags[cell] >= 0) {
                if (actual_type == Block_Types::Air || !in_tag(recipe.shaped_tags[cell], actual_type)) return false;
            } else if (actual_type != expected) {
                return false;
            }
        }
    }
    return true;
}

bool Recipe_Registry::match_shapeless(const Recipe& recipe, const std::array<Item_Stack, 9>& grid) const {
    std::vector<std::pair<Block_Types, int>> have;
    for (const Item_Stack& stack : grid) {
        if (stack.is_empty()) continue;
        bool found = false;
        for (auto& [type, count] : have) {
            if (type == stack.type) { count += 1; found = true; break; }
        }
        if (!found) have.emplace_back(stack.type, 1);
    }
    // Сначала точные ингредиенты, затем теги забирают всё оставшееся; лишних предметов быть не должно.
    for (const auto& [type, count] : recipe.ingredients) {
        const auto it = std::find_if(have.begin(), have.end(),
            [&](const auto& p) { return p.first == type; });
        if (it == have.end() || it->second != count) return false;
        it->second = 0;
    }
    for (const auto& [tag, count] : recipe.tag_ingredients) {
        int total = 0;
        for (auto& entry : have) {
            if (entry.second > 0 && in_tag(tag, entry.first)) { total += entry.second; entry.second = 0; }
        }
        if (total != count) return false;
    }
    for (const auto& entry : have) if (entry.second != 0) return false;
    return true;
}

const Recipe* Recipe_Registry::match(const std::array<Item_Stack, 9>& grid) const {
    for (const Recipe& recipe : m_recipes) {
        const bool matched = recipe.shapeless ? match_shapeless(recipe, grid) : match_shaped(recipe, grid);
        if (matched) return &recipe;
    }
    return nullptr;
}
