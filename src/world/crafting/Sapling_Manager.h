//
// Sapling_Manager — рост еловых саженцев (Block_Types::Spruce_Sapling).
//
// Саженцы ставит только игрок, на землю или дёрн. Группа — квадрат саженцев на одном уровне:
//   * 1x1 -> маленькая ель (обычная ель из генерации мира), 600-1200 тиков;
//   * 2x2 -> средняя ель (процедурная, высота 20-40, ширина до 18), 800-1900 тиков;
//   * 3x3 -> большая ель (процедурная, высота 35-80, ширина до 30), 2500-4200 тиков.
// Таймер (в тиках, 20 тиков = 1 секунда) выбирается случайно, когда группа складывается.
// Если квадрат из саженцев появился или распался, группа пересобирается и таймер берётся заново.
//
// Дерево растёт только если хватает места: все блоки кроны/ствола должны попасть в воздух,
// траву, цветы или листву. Иначе саженец ждёт и пробует снова через Retry_Ticks.
// Растут только группы в ЗАГРУЖЕННЫХ чанках. Состояние сохраняется в saplings.txt рядом с world.json.
//
#ifndef OPTICRAFT_SAPLING_MANAGER_H
#define OPTICRAFT_SAPLING_MANAGER_H

#include <functional>
#include <glm/glm.hpp>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "world/blocks/Block_Types.h"
#include "math/Hash_Utils.h"

class Chunk_Manager;

class Sapling_Manager {
public:
    Sapling_Manager();

    // --- Справочные функции ---
    // Саженцы всех видов. Группы по 2x2 / 3x3 собираются только из еловых саженцев.
    static bool is_sapling(Block_Types type);
    // Саженец растёт только на земле или дёрне.
    static bool can_grow_on(Block_Types ground);

    // Игрок поставил саженец в pos (блок в мире ставит вызывающий).
    void plant(const glm::ivec3& pos, Block_Types sapling);
    // Саженец в pos сломан или заменён (блок в мире меняет вызывающий).
    void remove(const glm::ivec3& pos);
    void clear();
    size_t size() const { return m_member_origin.size(); }

    // Двигает рост. В out_changed попадают позиции блоков, которые изменились (для обновления меша).
    void update(float delta_time, Chunk_Manager& chunk_manager, std::vector<glm::ivec3>& out_changed);

    std::string serialize() const;
    void deserialize(const std::string& text);

private:
    struct Group {
        int size = 1;            // сторона квадрата: 1, 2 или 3
        Block_Types species = Block_Types::Spruce_Sapling; // вид дерева, которое вырастет
        float progress = 0.0f;   // прошедшие тики
        float total = 600.0f;    // сколько тиков нужно до роста
        float retry = 0.0f;      // тики до следующей попытки, если место занято
    };

    static std::vector<glm::ivec3> cells_of(const glm::ivec3& origin, int size);
    bool is_cell(const glm::ivec3& pos, const glm::ivec3& origin, int size) const;
    bool group_loaded(const Chunk_Manager& chunk_manager, const glm::ivec3& origin, int size) const;

    float roll_total(int size, Block_Types species);
    void make_single(const glm::ivec3& pos, Block_Types species);
    void form_group_around(const glm::ivec3& pos);
    // Распускает группу: её саженцы, которые ещё живы (alive), снова становятся одиночными и
    // пробуют сложиться в группу с соседями.
    void release(const glm::ivec3& origin, const std::function<bool(const glm::ivec3&)>& alive);

    bool try_grow(const glm::ivec3& origin, int size, Chunk_Manager& chunk_manager,
                  std::vector<glm::ivec3>& out_changed);

    // pos саженца -> origin группы, которой он принадлежит
    std::unordered_map<glm::ivec3, glm::ivec3, Chunk_Key_Hash> m_member_origin;
    // origin группы -> состояние группы
    std::unordered_map<glm::ivec3, Group, Chunk_Key_Hash> m_groups;
    std::mt19937 m_rng;
};

#endif //OPTICRAFT_SAPLING_MANAGER_H
