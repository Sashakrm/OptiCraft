//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_BLOCK_TYPES_H
#define OPTICRAFT_BLOCK_TYPES_H

#include <string>
#include <cstdint>
#include <array>
#include <glm/glm.hpp>

using Block_ID = uint16_t;

// Именованные ID для блоков, на которые ссылается C++ код напрямую (генерация мира, физика и т.д.).
// Ёмкость реестра (см. Block_Registry) больше не привязана к этому enum — id 11 и выше можно
// добавлять чисто через assets/blocks.json без изменения C++ и без пересборки под новый Count.
enum class Block_Types : Block_ID {
    Air = 0,
    Grass = 1,
    Dirt = 2,
    Stone = 3,
    Sand = 4,
    Oak_Wood = 5,
    Oak_Leaf = 6,
    Water = 7,
    // 8 зарезервирован (см. "reserved_block_ids" в assets/blocks.json) под будущий персистентный ID.
    Rose = 9,
    Tall_Grass = 10,
    Sun_Stone = 11,

    Red_Sand = 12,
    Gravel = 13,
    Snow_Block = 14,
    Birch_Log = 15,
    Birch_Leaves = 17,
    Spruce_Log = 18,
    Spruce_Leaves = 20,
    Jungle_Log = 21,
    Jungle_Leaves = 23,
    Acacia_Log = 24,
    Acacia_Leaves = 26,
    Slimewood_Log = 27,
    Slimewood_Leaves = 28,
    Fern = 30,
    Flower = 31,
    Moss = 32,
    Mushroom = 33,
    Root = 34,
    Glow_Berry = 35,
    Mossy_Stone = 36,
    Iron_Ore = 37,
    Gold_Ore = 38,
    Rare_Mineral = 39,
    Cave_Crystal = 40,
    Slime_Grass = 41,
    Slime_Block = 42,
    Deep_Stone = 43,
    Lava_Stone = 44,
    Cave_Vine = 45,
    Mud = 46,
    Wet_Stone = 47,
    Cloud_Stone = 48,
    Ice = 49,

    // Блоки-контейнеры ("блок внутри блока" — см. Block_Entity.h): у каждого из них есть
    // Block_Entity с инвентарём, который живёт отдельно от самого Chunk_Block_Grid.
    Chest = 50,
    Crafting_Table = 51,
    Furnace = 52,

    // Предметы, которых нет в мире как блоков (в этом движке предмет = запись реестра блоков,
    // отдельного реестра предметов нет — см. Item_Stack в Inventory.h). id совпадают с blocks.json.
    Raw_Porkchop = 53,
    Cooked_Porkchop = 54,
    Charcoal = 55,

    // Земледелие. Стадии роста — отдельные блоки (так проще рисовать флору и хранить в чанках).
    Farmland = 59,          // грядка (сухая)
    Farmland_Wet = 60,      // грядка рядом с водой (только вид; рост считает Crop_Manager)
    Wild_Farmland = 61,     // дикая грядка из генерации мира — исчезает, когда урожай собран
    Wheat_Crop_0 = 62, Wheat_Crop_1 = 63, Wheat_Crop_2 = 64, Wheat_Crop_3 = 65,
    Carrot_Crop_0 = 66, Carrot_Crop_1 = 67, Carrot_Crop_2 = 68, Carrot_Crop_3 = 69,

    // Предметы и материалы.
    Wheat_Seeds = 70,
    Wheat = 71,
    Carrot = 72,
    Raw_Beef = 73,
    Cooked_Beef = 74,
    Raw_Horse_Meat = 75,
    Cooked_Horse_Meat = 76,
    Stick = 77,
    Iron_Ingot = 78,
    Gold_Ingot = 79,
    Diamond = 80,
    Diamond_Ore = 81,
    // Инструменты 82..101 — см. blocks.json (4 материала x 5 видов).

    Count = 102
};

// Максимальная ёмкость реестра блоков: данные из blocks.json могут занимать любые id в этом
// диапазоне, включая id, для которых нет именованной константы в Block_Types.
inline constexpr Block_ID k_max_block_types = 256;

// Вид инструмента. Для БЛОКА поле effective_tool говорит, каким инструментом он копается быстрее.
enum class Tool_Type : uint8_t { None = 0, Pickaxe, Axe, Shovel, Sword, Hoe };

enum class Block_Mesh_Type : uint8_t {
    Solid = 0,
    Flora = 1,
    Liquid = 2
};

enum class Block_Mesh_Style : uint8_t {
    Block = 0,
    XStyle = 1
};

enum class Block_State : uint8_t {
    Solid = 0,
    Liquid = 1,
    Gas = 2
};

// --- Метаданные блока (1 байт на блок, хранятся разреженно — см. Chunk_Block_Grid) ---------
//   биты 0..1 — "направление": для Axis-блоков (бревно) 0 = вертикально (Y), 1 = вдоль X,
//               2 = вдоль Z; для Facing-блоков (печь, сундук, верстак) — в какую
//               сторону смотрит лицевая грань: 0 = -Z, 1 = +X, 2 = +Z, 3 = -X;
//   бит 3     — "активен" (печь горит): включает набор active_texture из blocks.json.
namespace Block_Meta {
    constexpr uint8_t direction_mask = 0x03;
    constexpr uint8_t active_bit = 0x08;

    constexpr uint8_t axis_y = 0, axis_x = 1, axis_z = 2;
    constexpr uint8_t facing_north = 0, facing_east = 1, facing_south = 2, facing_west = 3; // -Z,+X,+Z,-X
}

// Индексы граней в таблицах текстур: +X, -X, +Y, -Y, +Z, -Z.
namespace Block_Face {
    constexpr int pos_x = 0, neg_x = 1, pos_y = 2, neg_y = 3, pos_z = 4, neg_z = 5;
    constexpr int count = 6;
}

enum class Block_Orientation : uint8_t {
    None = 0,   // одинаково со всех сторон (или сторона задаётся только текстурами)
    Axis = 1,   // бревно: ось зависит от грани, на которую поставили ("orientation": "axis")
    Facing = 2  // печь/сундук/верстак: лицом к игроку ("orientation": "facing")
};

// Текстуры граней одного блока в одном состоянии (обычном или активном).
struct Block_Face_Textures {
    glm::vec4 top{0.0f}, side{0.0f}, bottom{0.0f};
    // Лицевая грань для Facing-блоков. Если не задана — лица нет, блок выглядит как обычный.
    glm::vec4 front{0.0f};
    bool has_front = false;
    // Точечные переопределения отдельных граней (ключи north/south/east/west в JSON).
    std::array<glm::vec4, Block_Face::count> override_uv{};
    std::array<bool, Block_Face::count> has_override{};
};

struct Block_Properties {
    std::string name;
    bool is_solid;
    bool is_transparent;
    bool is_opaque;
    bool can_update;
    float hardness;
    Block_Mesh_Type mesh_type;
    Block_Mesh_Style mesh_style;
    Block_State state;

    // UV-прямоугольники (u_min,v_min,u_max,v_max), уже разрешённые из
    // "atlas"+имя спрайта в blocks.json через Atlas_Registry на этапе
    // загрузки (см. Block_Registry::load_from_file) — ни Chunk.cpp, ни
    // что-либо ещё в рендере НЕ обращается к атласу по индексу напрямую,
    // только читает готовый прямоугольник отсюда. Так один блок может
    // ссылаться на спрайт произвольного пиксельного размера (не 16x16),
    // и в будущем — даже из разных атласов на разные грани, если понадобится.
    glm::vec4 uv_top{0.0f};    // Для верхней грани (трава)
    glm::vec4 uv_side{0.0f};   // Для боковых граней
    glm::vec4 uv_bottom{0.0f}; // Для нижней грани (корень травы)

    // Уровень испускаемого света, 0..14 (см. Config::max_light_level в Chunk_Manager) —
    // 0 значит "не светится". Sun Stone — первый и пока единственный блок с ненулевым
    // значением; распространение см. Chunk_Manager::propagate_lighting.
    int light_emission = 0;

    // --- Выживание (всё задаётся в blocks.json, значения по умолчанию = "обычный блок") ---
    // Предмет, а не блок: не ставится в мир (см. Block_Interaction), в мире выпавший предмет
    // рисуется плоским спрайтом, а не кубиком.
    bool is_item = false;
    // Еда: сколько очков голода восстанавливает (0 = несъедобно) и насыщение (как в Minecraft).
    int food_points = 0;
    float food_saturation = 0.0f;
    // Что выпадает при добыче в Survival: -1 = сам блок, 0 = ничего, >0 = id другого блока/предмета.
    int drop_id = -1;
    int drop_count = 1;
    float drop_chance = 1.0f;
    // Время добычи голыми руками в секундах. < 0 = считается из hardness
    // (см. Config::survival_break_seconds_per_hardness), 0 = мгновенно.
    float break_time = -1.0f;

    // --- Инструменты (всё из blocks.json; у обычных блоков нули) ---
    int max_durability = 0;            // > 0 — это инструмент с прочностью, не стакается
    Tool_Type tool_type = Tool_Type::None; // чем является предмет
    int tool_tier = 0;                 // 1 дерево, 2 камень, 3 железо, 4 алмаз
    float tool_speed = 1.0f;           // множитель скорости добычи "своих" блоков
    float attack_damage = 0.0f;        // урон по мобам (0 = как кулак)
    Tool_Type effective_tool = Tool_Type::None; // для блока: лучший инструмент
    int harvest_tier = 0;              // для блока: минимальный tier кирки, чтобы получить дроп
    bool show_in_creative = true;      // false — не показывать в творческом инвентаре

    // --- Ориентация и состояния (см. Block_Meta) ---
    Block_Orientation orientation = Block_Orientation::None;
    bool has_active_state = false;   // есть отдельный набор текстур "active_texture"
    // Нужно ли вообще читать метаданные при построении меша: у обычных блоков (камень, земля...)
    // ответ "нет", и мешинг идёт по прежнему быстрому пути без единого обращения к карте.
    bool uses_meta = false;
    Block_Face_Textures textures;         // обычное состояние
    Block_Face_Textures active_textures;  // активное состояние (если has_active_state)
    // Иконка в инвентаре/хотбаре: лицо блока, если оно есть, иначе верхняя грань.
    glm::vec4 uv_icon{0.0f};

    // UV грани для ЭТИХ метаданных. Возвращает также, нужен ли поворот текстуры на 90°
    // (бревно, лежащее на боку: кора должна идти вдоль ствола).
    const glm::vec4& resolve_face_uv(int face, uint8_t meta, bool& rotate_quarter) const;

    // UV нужной грани; face_direction: +1 = верх (+Y), -1 = низ (-Y), 0 = бок.
    const glm::vec4& get_uv(int face_direction) const {
        if (face_direction == 1) return uv_top;
        if (face_direction == -1) return uv_bottom;
        return uv_side;
    }
};

class Block_Registry {
private:
    std::array<Block_Properties, k_max_block_types> m_properties;
    std::array<bool, k_max_block_types> m_reserved{};
    Block_ID m_registered_block_count = 0;

    // Плоские таблицы "id -> нужный нам один байт", построенные один раз в конструкторе
    // (см. build_fast_tables) сразу после того, как m_properties окончательно заполнен
    // load_defaults()+load_from_file(). Реестр больше никогда не перезагружается в рантайме
    // (единственный вызов — Block_Registry::get_instance() в конструкторе, один раз за
    // сессию, см. Game.cpp), так что кэш безопасно строить один раз и не инвалидировать.
    //
    // Зачем это вообще нужно: BFS освещения (Chunk_Manager::recompute_lighting и
    // recompute_sky_lighting) на КАЖДЫЙ воксель спрашивает "он прозрачный?" и "он светится?".
    // get_properties(id) возвращает ссылку на Block_Properties — а это тяжёлая структура
    // (std::string name, три glm::vec4 под UV и т.д., далеко за 100 байт), то есть чтобы
    // прочитать один bool, мы каждый раз тащим в кэш-линии чужие поля, которые BFS вообще
    // не использует. Плюс сам get_properties(Block_Types) идёт через get_instance() —
    // Meyer's singleton, а это проверка guard-переменной инициализации на каждый вызов.
    // Таблицы ниже — 256 байт каждая, гарантированно целиком в L1 на всё время обхода,
    // и вызывающий код держит на них голый указатель, а не дёргает singleton в цикле.
    std::array<bool, k_max_block_types> m_transparent_flat{};
    std::array<uint8_t, k_max_block_types> m_light_emission_flat{};
    void build_fast_tables();

    Block_Registry();
    void load_defaults();
    void load_from_file(const std::string& path);
    void set_properties(Block_ID id, const Block_Properties& properties);

public:
    static Block_Registry& get_instance();

    const Block_Properties& get_properties(Block_Types type) const;
    const Block_Properties& get_properties(Block_ID id) const;
    Block_ID get_block_id(const std::string& name) const;
    bool is_reserved(Block_ID id) const;

    // Индекс — Block_ID напрямую (0..255), БЕЗ проверки границ: Chunk_Block_Grid хранит id
    // блока как uint8_t, так что значение, прочитанное из чанка, всегда влезает в [0,255] —
    // ровно размер этих таблиц. Проверка границ, которая есть в get_properties(id), здесь
    // не нужна и не нужен singleton-геттер: вызывающий код один раз берёт указатель на
    // таблицу ДО цикла BFS (get_instance() вызывается один раз, не на каждый воксель).
    const bool* transparent_table() const { return m_transparent_flat.data(); }
    const uint8_t* light_emission_table() const { return m_light_emission_flat.data(); }

    // Число id, реально занятых блоками (именованными или чисто data-driven из blocks.json).
    // Используется вместо Block_Types::Count там, где список блоков должен обновляться
    // при правке blocks.json без пересборки движка (например, инвентарь).
    Block_ID get_registered_block_count() const { return m_registered_block_count; }
};

inline const Block_Properties& get_block_props(Block_Types type) {
    return Block_Registry::get_instance().get_properties(type);
}

inline const Block_Properties& get_block_props(Block_ID id) {
    return Block_Registry::get_instance().get_properties(id);
}

#endif //OPTICRAFT_BLOCK_TYPES_H
