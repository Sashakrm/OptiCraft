//
// Crop_Manager — рост посевов (пшеница, морковь) на грядках.
//
// Стадии роста — это отдельные блоки (Wheat_Crop_0..3, Carrot_Crop_0..3), а сам таймер роста живёт
// здесь: на каждый посаженный росток при посадке случайно выбирается полное время роста
// (Config::crop_grow_seconds_min..max, 3-6 минут). Если рядом с грядкой есть вода (в пределах
// Config::farmland_water_radius блоков), рост идёт в Config::crop_wet_growth_multiplier раза быстрее,
// а сама грядка рисуется «влажной» (Farmland_Wet).
//
// Растут только посевы в ЗАГРУЖЕННЫХ чанках (как в Minecraft). Состояние сохраняется в crops.txt
// рядом с world.json. Дикие морковки из генерации мира (уже спелые) в менеджере не числятся.
//
#ifndef OPTICRAFT_CROP_MANAGER_H
#define OPTICRAFT_CROP_MANAGER_H

#include <glm/glm.hpp>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "world/blocks/Block_Types.h"
#include "math/Hash_Utils.h"

class Chunk_Manager;

enum class Crop_Kind : uint8_t { Wheat = 0, Carrot = 1 };

class Crop_Manager {
public:
    Crop_Manager();

    // --- Справочные функции (без состояния) ---
    static bool is_crop_block(Block_Types type);
    static Crop_Kind kind_of(Block_Types crop_block);
    static int stage_of(Block_Types crop_block);              // 0..3
    static Block_Types crop_block(Crop_Kind kind, int stage); // stage 0..3
    static bool is_mature(Block_Types crop_block) { return stage_of(crop_block) >= 3; }
    static bool is_farmland(Block_Types type);                // любая грядка (сухая/мокрая/дикая)
    // Есть ли блок воды в радиусе Config::farmland_water_radius от грядки (по горизонтали, ±1 по высоте).
    static bool has_water_nearby(const Chunk_Manager& chunk_manager, const glm::ivec3& farmland_pos);

    // Посадить росток в pos (блок в мире ставит вызывающий). Время роста выбирается случайно.
    void plant(const glm::ivec3& pos, Crop_Kind kind);
    void remove(const glm::ivec3& pos);
    void clear() { m_crops.clear(); }
    size_t size() const { return m_crops.size(); }

    // Двигает рост. В out_changed попадают позиции блоков, которые изменились (для обновления меша).
    void update(float delta_time, Chunk_Manager& chunk_manager, std::vector<glm::ivec3>& out_changed);

    std::string serialize() const;
    void deserialize(const std::string& text);

private:
    struct Crop {
        Crop_Kind kind = Crop_Kind::Wheat;
        float progress = 0.0f;   // накопленное «время роста», секунды
        float total = 240.0f;    // сколько нужно набрать до спелости
        float wet_timer = 0.0f;  // до следующей проверки воды
        bool wet = false;
    };
    std::unordered_map<glm::ivec3, Crop, Chunk_Key_Hash> m_crops;
    std::mt19937 m_rng;
};

#endif //OPTICRAFT_CROP_MANAGER_H
