//
// Created by noktemor on 28.03.2026.
//

#include "Chunk_Manager.h"
#include "utils/Logger.h"
#include <cmath>

Chunk_Manager::Chunk_Manager(int load_radius)
    : m_player_chunk(0, 0)
    , m_load_radius(load_radius)
    , m_world_generator(std::make_unique<World_Generator>(Config::world_seed))  // ✅ Инициализация
{}


bool Chunk_Manager::is_chunk_loaded(int cx, int cz) const {
    return m_chunks.find({cx, cz}) != m_chunks.end();
}

void Chunk_Manager::unload_chunk(int cx, int cz) {
    auto it = m_chunks.find({cx, cz});
    if (it != m_chunks.end()) {
        m_chunks.erase(it);
    }
}

void Chunk_Manager::load_around(int player_cx, int player_cz) {
    for (int dx = -m_load_radius; dx <= m_load_radius; ++dx) {
        for (int dz = -m_load_radius; dz <= m_load_radius; ++dz) {
            int cx = player_cx + dx;
            int cz = player_cz + dz;

            if (!is_chunk_loaded(cx, cz)) {
                auto chunk = std::make_unique<Chunk>(cx, cz);

                // Генерация через World_Generator
                m_world_generator->generate_chunk(chunk.get());
                chunk->build_mesh();

                m_chunks[{cx, cz}] = std::move(chunk);
            }
        }
    }
}

void Chunk_Manager::unload_distant(int center_cx, int center_cz, int max_distance) {
    std::vector<glm::ivec2> to_unload;

    for (const auto& [key, chunk] : m_chunks) {
        int dx = std::abs(key.x - center_cx);
        int dz = std::abs(key.y - center_cz);
        if (dx > max_distance || dz > max_distance) {
            to_unload.push_back(key);
        }
    }

    for (const auto& key : to_unload) {
        unload_chunk(key.x, key.y);
    }
}

void Chunk_Manager::update_player_position(float player_x, float player_z) {
    int player_cx = static_cast<int>(std::floor(player_x / Config::chunk_size));
    int player_cz = static_cast<int>(std::floor(player_z / Config::chunk_size));

    if (player_cx == m_player_chunk.x && player_cz == m_player_chunk.y) {
        return;
    }

    m_player_chunk = {player_cx, player_cz};

    unload_distant(player_cx, player_cz, m_load_radius + 1);
    load_around(player_cx, player_cz);

    // ✅ УДАЛЕНО: код с chunk, которого здесь не было
}

Chunk* Chunk_Manager::get_chunk(int cx, int cz) {
    auto it = m_chunks.find({cx, cz});
    return it != m_chunks.end() ? it->second.get() : nullptr;
}

const Chunk* Chunk_Manager::get_chunk(int cx, int cz) const {
    auto it = m_chunks.find({cx, cz});
    return it != m_chunks.end() ? it->second.get() : nullptr;
}

std::vector<std::pair<glm::ivec2, std::vector<Chunk_Vertex>>> Chunk_Manager::get_renderable_chunks() const {
    std::vector<std::pair<glm::ivec2, std::vector<Chunk_Vertex>>> result;

    for (const auto& [key, chunk] : m_chunks) {
        if (!chunk->is_empty()) {
            result.push_back({key, chunk->get_vertices()});
        }
    }

    return result;
}