#include "world/crafting/Crop_Manager.h"
#include "world/chunks/Chunk_Manager.h"
#include "utils/Config.h"
#include "utils/logging/Logger.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <cstdlib>

Crop_Manager::Crop_Manager() : m_rng(std::random_device{}()) {}

bool Crop_Manager::is_crop_block(Block_Types type) {
    const int id = static_cast<int>(type);
    return id >= static_cast<int>(Block_Types::Wheat_Crop_0) && id <= static_cast<int>(Block_Types::Carrot_Crop_3);
}

Crop_Kind Crop_Manager::kind_of(Block_Types crop_block) {
    return static_cast<int>(crop_block) >= static_cast<int>(Block_Types::Carrot_Crop_0)
        ? Crop_Kind::Carrot : Crop_Kind::Wheat;
}

int Crop_Manager::stage_of(Block_Types crop_block) {
    const int base = kind_of(crop_block) == Crop_Kind::Carrot
        ? static_cast<int>(Block_Types::Carrot_Crop_0) : static_cast<int>(Block_Types::Wheat_Crop_0);
    return std::clamp(static_cast<int>(crop_block) - base, 0, 3);
}

Block_Types Crop_Manager::crop_block(Crop_Kind kind, int stage) {
    const int base = kind == Crop_Kind::Carrot
        ? static_cast<int>(Block_Types::Carrot_Crop_0) : static_cast<int>(Block_Types::Wheat_Crop_0);
    return static_cast<Block_Types>(base + std::clamp(stage, 0, 3));
}

bool Crop_Manager::is_farmland(Block_Types type) {
    return type == Block_Types::Farmland || type == Block_Types::Farmland_Wet ||
           type == Block_Types::Wild_Farmland;
}

bool Crop_Manager::has_water_nearby(const Chunk_Manager& chunk_manager, const glm::ivec3& pos) {
    constexpr int r = Config::farmland_water_radius;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                if (chunk_manager.get_block_world(pos.x + dx, pos.y + dy, pos.z + dz) == Block_Types::Water) {
                    return true;
                }
            }
        }
    }
    return false;
}

void Crop_Manager::plant(const glm::ivec3& pos, Crop_Kind kind) {
    std::uniform_real_distribution<float> dist(Config::crop_grow_seconds_min, Config::crop_grow_seconds_max);
    Crop crop;
    crop.kind = kind;
    crop.total = dist(m_rng);
    crop.wet_timer = 0.0f; // сразу проверим воду
    m_crops[pos] = crop;
}

void Crop_Manager::remove(const glm::ivec3& pos) {
    m_crops.erase(pos);
}

void Crop_Manager::update(float delta_time, Chunk_Manager& chunk_manager, std::vector<glm::ivec3>& out_changed) {
    // Отладка: OPTICRAFT_CROP_SPEED=N ускоряет рост в N раз (для автотестов).
    static const float k_debug_speed = [] {
        const char* v = std::getenv("OPTICRAFT_CROP_SPEED");
        return v ? std::max(1.0f, static_cast<float>(std::atof(v))) : 1.0f;
    }();
    delta_time *= k_debug_speed;
    std::vector<glm::ivec3> to_remove;
    for (auto& [pos, crop] : m_crops) {
        const glm::ivec3 chunk = chunk_manager.get_chunk_coords_for_world(pos.x, pos.y, pos.z);
        if (!chunk_manager.is_chunk_loaded(chunk.x, chunk.y, chunk.z)) continue; // не растёт вне загруженной зоны

        const Block_Types here = chunk_manager.get_block_world(pos.x, pos.y, pos.z);
        const glm::ivec3 below_pos(pos.x, pos.y - 1, pos.z);
        const Block_Types below = chunk_manager.get_block_world(below_pos.x, below_pos.y, below_pos.z);

        // Росток сломали (или заменили) — запись больше не нужна.
        if (!is_crop_block(here) || kind_of(here) != crop.kind) { to_remove.push_back(pos); continue; }
        // Грядку выбили из-под ростка — росток падает.
        if (!is_farmland(below)) {
            if (chunk_manager.set_block_world(pos.x, pos.y, pos.z, Block_Types::Air)) out_changed.push_back(pos);
            to_remove.push_back(pos);
            continue;
        }

        crop.wet_timer -= delta_time;
        if (crop.wet_timer <= 0.0f) {
            crop.wet_timer = 2.0f;
            crop.wet = has_water_nearby(chunk_manager, below_pos);
            // Вид грядки следует за влажностью (дикая грядка остаётся как есть).
            const Block_Types wanted = crop.wet ? Block_Types::Farmland_Wet : Block_Types::Farmland;
            if ((below == Block_Types::Farmland || below == Block_Types::Farmland_Wet) && below != wanted) {
                if (chunk_manager.set_block_world(below_pos.x, below_pos.y, below_pos.z, wanted)) {
                    out_changed.push_back(below_pos);
                }
            }
        }

        crop.progress += delta_time * (crop.wet ? Config::crop_wet_growth_multiplier : 1.0f);
        const int stage = crop.progress >= crop.total ? 3
                        : std::clamp(static_cast<int>(crop.progress / crop.total * 4.0f), 0, 3);
        if (stage != stage_of(here)) {
            if (chunk_manager.set_block_world(pos.x, pos.y, pos.z, crop_block(crop.kind, stage))) {
                out_changed.push_back(pos);
                LOG_INFO("Crop at " + std::to_string(pos.x) + " " + std::to_string(pos.y) + " " + std::to_string(pos.z) +
                         " grew to stage " + std::to_string(stage) + (crop.wet ? " (wet)" : " (dry)"));
            }
        }
        // Спелый росток дальше не растёт — запись можно убрать (блок остаётся в мире).
        if (stage >= 3 && crop.progress >= crop.total) to_remove.push_back(pos);
    }
    for (const glm::ivec3& pos : to_remove) m_crops.erase(pos);
}

std::string Crop_Manager::serialize() const {
    std::ostringstream out;
    for (const auto& [pos, crop] : m_crops) {
        out << pos.x << ' ' << pos.y << ' ' << pos.z << ' ' << static_cast<int>(crop.kind) << ' '
            << crop.progress << ' ' << crop.total << '\n';
    }
    return out.str();
}

void Crop_Manager::deserialize(const std::string& text) {
    m_crops.clear();
    std::istringstream in(text);
    glm::ivec3 pos;
    int kind = 0;
    float progress = 0.0f, total = 0.0f;
    while (in >> pos.x >> pos.y >> pos.z >> kind >> progress >> total) {
        Crop crop;
        crop.kind = kind == 1 ? Crop_Kind::Carrot : Crop_Kind::Wheat;
        crop.progress = std::max(0.0f, progress);
        crop.total = std::max(1.0f, total);
        m_crops[pos] = crop;
    }
}
