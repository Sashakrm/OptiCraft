//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_CHUNK_MANAGER_H
#define OPTICRAFT_CHUNK_MANAGER_H

#include <unordered_map>
#include <memory>
#include <glm/glm.hpp>
#include "Chunk.h"
#include "utils/Hash_Utils.h"


class Chunk_Manager {
private:
    std::unordered_map<glm::ivec2, std::unique_ptr<Chunk>, Chunk_Key_Hash> m_chunks;

    glm::ivec2 m_player_chunk;
    int m_load_radius;

    bool is_chunk_loaded(int cx, int cz) const;
    void unload_chunk(int cx, int cz);

public:
    Chunk_Manager(int load_radius = Config::chunk_load_radius);

    // Загрузка/выгрузка
    void load_around(int player_cx, int player_cz);
    void unload_distant(int center_cx, int center_cz, int max_distance);

    // Обновление
    void update_player_position(float player_x, float player_z);

    // Доступ к чанкам
    Chunk* get_chunk(int cx, int cz);
    const Chunk* get_chunk(int cx, int cz) const;

    // Для рендера
    std::vector<std::pair<glm::ivec2, std::vector<Chunk_Vertex>>> get_renderable_chunks() const;

    // Статистика
    size_t get_loaded_count() const { return m_chunks.size(); }
};

#endif //OPTICRAFT_CHUNK_MANAGER_H
