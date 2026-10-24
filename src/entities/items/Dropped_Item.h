//
// Dropped_Item — предмет, лежащий в мире: выпал из сломанного блока, из убитого моба или
// из разрушенного контейнера. Как в Minecraft: подпрыгивает, падает, скользит по земле,
// крутится и покачивается, сливается с такими же соседями, подбирается игроком и сам
// исчезает через Config::dropped_item_lifetime секунд, если его никто не взял.
//
// Предмет в этом движке — запись реестра блоков (см. Item_Stack в Inventory.h), поэтому
// выпавший блок рисуется маленьким кубиком с теми же текстурами, что и в мире, а предмет
// без блока (мясо, уголь) и «цветочные» блоки — плоским спрайтом.
//
// Ничего не знает про OpenGL: build_mesh() лишь наполняет массив Chunk_Vertex, который затем
// рисует Renderer::render_dropped_items() тем же атласом блоков. Поэтому весь класс
// проверяется без окна и GL-контекста.
//
#ifndef OPTICRAFT_DROPPED_ITEM_H
#define OPTICRAFT_DROPPED_ITEM_H

#include <functional>
#include <glm/glm.hpp>
#include <random>
#include <vector>
#include "entities/player/Inventory.h" // Item_Stack
#include "utils/Config.h"
#include "world/chunks/Chunk.h"        // Chunk_Vertex

class Chunk_Manager;

struct Dropped_Item {
    Item_Stack stack;
    glm::vec3 position{0.0f};  // центр НИЖНЕЙ грани хитбокса (как у игрока и свиней)
    glm::vec3 velocity{0.0f};
    float age = 0.0f;          // секунд с момента выпадения; по достижении lifetime удаляется
    float pickup_delay = 0.0f; // пока > 0 — подобрать нельзя
    float phase = 0.0f;        // сдвиг фазы вращения/покачивания, чтобы предметы не крутились в унисон
    bool on_ground = false;

    // Брошенный зелье-шарик: летит, не подбирается и не сливается, разбивается о блок или моба.
    bool thrown = false;
    bool shattered = false;    // разбился — будет удалён на этом же шаге

    // Предмет уже отдан инвентарю и «долетает» до игрока (чисто визуально), затем исчезает.
    bool collecting = false;
    float collect_time = 0.0f;
    glm::vec3 collect_from{0.0f};
};

// Что выпадает из блока при добыче в Survival (учитывает поля drop/drop_count/drop_chance из
// blocks.json). Пустой стак — ничего не выпало.
Item_Stack roll_block_drop(Block_Types block, std::mt19937& rng);

// Место, где брошенный шарик разбился, и его тип (по нему выбирается эффект).
struct Thrown_Hit {
    glm::vec3 position;
    Block_Types type;
};

class Dropped_Item_Manager {
public:
    Dropped_Item_Manager();

    // Брошенный шарик: летит по баллистике с гравитацией. Разбивается, когда задел блок,
    // когда mob_hit(позиция) вернул true (моб рядом) или по истечении potion_thrown_lifetime.
    void spawn_thrown(const Item_Stack& stack, const glm::vec3& position, const glm::vec3& velocity);
    // Забирает места разбитых шариков (и очищает список).
    std::vector<Thrown_Hit> take_thrown_hits();

    // Кладёт предмет в мир. Если предметов уже слишком много (Config::dropped_item_max_count),
    // самые старые удаляются.
    void spawn(const Item_Stack& stack, const glm::vec3& position, const glm::vec3& velocity,
               float pickup_delay = Config::dropped_item_pickup_delay);

    // Дроп разрушенного блока: обычный дроп по таблице + всё содержимое контейнера
    // (сундук/печь/верстак), которое иначе пропало бы вместе с блоком.
    void spawn_block_drops(const glm::ivec3& block_pos, Block_Types block,
                           const std::vector<Item_Stack>& container_contents);

    // Дроп из моба: слегка подбрасывается вверх и в стороны.
    void spawn_from_mob(const glm::vec3& mob_position, const Item_Stack& stack);

    // Физика, слияние, подбор, время жизни. pickup_inventory == nullptr — подбирать нельзя
    // (Creative или игрок мёртв): предметы просто лежат и исчезают по таймеру.
    void update(float delta_time,
                const Chunk_Manager& chunk_manager,
                const glm::vec3& player_position,
                float player_height,
                Inventory* pickup_inventory,
                const std::function<bool(const glm::vec3&)>& mob_hit = {});

    // Добавляет в out вершины всех предметов (кубики/спрайты) в мировых координатах.
    void build_mesh(std::vector<Chunk_Vertex>& out, const Chunk_Manager& chunk_manager) const;

    size_t size() const { return m_items.size(); }
    const std::vector<Dropped_Item>& get_items() const { return m_items; }
    void clear() { m_items.clear(); }

private:
    std::vector<Dropped_Item> m_items;
    std::vector<Thrown_Hit> m_thrown_hits;
    std::mt19937 m_rng;
    float m_time = 0.0f;
    float m_merge_timer = 0.0f;
    glm::vec3 m_collect_target{0.0f}; // куда летят подбираемые предметы (грудь игрока)

    bool collides_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const;
    bool is_chunk_loaded_at(const glm::vec3& position, const Chunk_Manager& chunk_manager) const;
    void simulate(Dropped_Item& item, float delta_time, const Chunk_Manager& chunk_manager);
    void try_pickup(Dropped_Item& item, const glm::vec3& player_position, float player_height,
                    Inventory& inventory);
    void merge_nearby();
    void simulate_thrown(Dropped_Item& item, float delta_time, const Chunk_Manager& chunk_manager,
                         const std::function<bool(const glm::vec3&)>& mob_hit);
    void enforce_limit();
};

#endif //OPTICRAFT_DROPPED_ITEM_H
