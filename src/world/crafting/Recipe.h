//
// Recipe_Registry — зачатки крафта для верстака (см. Block_Entity_Type::Crafting_Table).
// Читает assets/recipe.json и умеет сопоставить содержимое сетки 3x3 с рецептом.
// Поддерживает два вида рецептов, как в Minecraft:
//   - "shapeless" — важен только набор и количество ингредиентов, не расположение;
//   - "shaped" — важна форма узора (проверяется по ограничивающему прямоугольнику
//     непустых ячеек, без поворотов/отражений — этого достаточно для зачатков).
// Выход рецепта ничего не делает сам по себе — совпадение просто говорит вызывающему
// коду (Container_Session), какой блок и в каком количестве выдать, если игрок кликнет
// по слоту результата.
//
#ifndef OPTICRAFT_RECIPE_H
#define OPTICRAFT_RECIPE_H

#include <array>
#include <string>
#include <vector>
#include "world/blocks/Block_Types.h"
#include "entities/player/Inventory.h" // Item_Stack

struct Recipe {
    std::string name;
    bool shapeless = false;

    // shaped: 3x3 узор, Block_Types::Air = пусто. Сравнивается по обрезанному
    // до непустых ячеек прямоугольнику — см. Recipe_Registry::match_shaped.
    // Ячейка может быть и «тегом» (любые доски, любые брёвна): тогда shaped_tags[i] != -1
    // и подходит любой блок из m_tags[shaped_tags[i]].
    std::array<Block_Types, 9> shaped_grid{};
    std::array<int, 9> shaped_tags{-1, -1, -1, -1, -1, -1, -1, -1, -1};
    int shaped_width = 0;
    int shaped_height = 0;

    // shapeless: тип -> требуемое количество (суммарно по всей сетке).
    std::vector<std::pair<Block_Types, int>> ingredients;
    // shapeless: теги (индекс тега -> требуемое количество).
    std::vector<std::pair<int, int>> tag_ingredients;

    Block_Types output_type = Block_Types::Air;
    int output_count = 1;
};

class Recipe_Registry {
public:
    static Recipe_Registry& get_instance();

    // grid — ровно 9 элементов, верстачная сетка 3x3 по строкам (см. Block_Entity_Slots::crafting_table).
    // nullptr, если ни один рецепт не подошёл.
    const Recipe* match(const std::array<Item_Stack, 9>& grid) const;

private:
    Recipe_Registry();
    void load_defaults();
    void load_from_file(const std::string& path);

    bool match_shaped(const Recipe& recipe, const std::array<Item_Stack, 9>& grid) const;
    bool match_shapeless(const Recipe& recipe, const std::array<Item_Stack, 9>& grid) const;
    bool in_tag(int tag, Block_Types type) const;

    std::vector<Recipe> m_recipes;
    // Теги из recipe.json ("tags": {"planks": ["Oak Planks", ...]}); в ключах рецептов пишутся как "#planks".
    std::vector<std::vector<Block_Types>> m_tags;
};

#endif //OPTICRAFT_RECIPE_H
