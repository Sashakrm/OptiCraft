//
// Smelting_Registry — что и как долго можно жарить в печи и чем её топить. Читает
// assets/smelting.json (рядом с assets/recipe.json, тот же принцип: имена предметов из
// blocks.json, без пересборки движка). Логика самой печи — Chunk_Manager::update_block_entities.
//
#ifndef OPTICRAFT_SMELTING_H
#define OPTICRAFT_SMELTING_H

#include <string>
#include <unordered_map>
#include "world/blocks/Block_Types.h"

struct Smelting_Recipe {
    Block_Types input = Block_Types::Air;
    Block_Types output = Block_Types::Air;
    int output_count = 1;
    float time_seconds = 10.0f; // сколько горящая печь готовит один предмет (как в Minecraft — 10 с)
};

class Smelting_Registry {
public:
    static Smelting_Registry& get_instance();

    // nullptr — этот предмет в печи не жарится.
    const Smelting_Recipe* find(Block_Types input) const;
    // Сколько секунд горит одна штука топлива; 0 — не топливо.
    float get_fuel_time(Block_Types item) const;

private:
    Smelting_Registry();
    void load_from_file(const std::string& path);

    std::unordered_map<Block_ID, Smelting_Recipe> m_recipes;
    std::unordered_map<Block_ID, float> m_fuel_seconds;
};

#endif //OPTICRAFT_SMELTING_H
