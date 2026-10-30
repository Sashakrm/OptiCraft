#ifndef OPTICRAFT_WORLD_GENERATOR_H
#define OPTICRAFT_WORLD_GENERATOR_H

#include "world/blocks/Block_Types.h"
#include "world/dimensions/Dimension.h"
#include "utils/Config.h"
#include "OpenSimplex2S.h"
#include "math/Fast_Rng.h"
#include "world/generation/Cave_Network.h"
#include "world/generation/River_Network.h"
#include "world/generation/Structure_Placement.h"

#include <memory>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

class Chunk;
class Chunk_Block_Grid;

// Биомы теперь идентифицируются числом, а не строкой. Раньше find_biome/get_surface_block
// сравнивали std::string(b.name) == "Forest" — это конструировало временную строку на каждый
// блок и на каждый из 23 биомов в цикле. Enum убирает и аллокации, и сравнения строк.
enum class Biome_Id : uint8_t {
    Ocean = 0,
    Shallow_Water,
    Beach,
    Plains,
    Forest,
    Birch_Forest,
    Taiga,
    Jungle,
    Savanna,
    Desert,
    Stone_Desert,
    Steppe,
    Tundra,
    Ice_Fields,
    Mountains,
    High_Mountains,
    Plateau,
    Slime,
    Solar,
    Sky,
    Underground,
    Deep_Underground,
    Flooded_Underground,
    Count
};

const char* biome_name(Biome_Id id);
// Биом по имени из biome_name(); неизвестное имя → Plains.
Biome_Id biome_from_name(const std::string& name);
// Биомы, которые можно выбрать для мира из одного биома (наземные, без воды и подземных).
const std::vector<Biome_Id>& selectable_single_biomes();

struct World_Sample {
    // Шесть климатических параметров BiomeTree.
    double temperature = 0.0;
    double humidity = 0.0;
    double continentalness = 0.0; // [-1, +1]
    double erosion = 0.0;
    double weirdness = 0.0;
    double depth = 0.0;           // относительно сгенерированной поверхности

    // Сигналы рельефа/связности — это условия, а не измерения BiomeTree.
    double surface_height = 0.0;
    double mountainness = 0.0;
    double cave_density = 0.0;
    double water_connection = 0.0;
    double surface_connection = 0.0;
    double slope = 0.0;           // перепад высоты с соседними колонками, в блоках

    int world_x = 0;
    int world_y = 0;
    int world_z = 0;
    bool near_ocean = false;
    bool sky_anchor_valid = false;
};

class World_Generator {
public:
    struct Prepared_Column; // Immutable shared context for parallel per-chunk filling.
    explicit World_Generator(const std::string& generation_folder = "Classic",
                             uint32_t seed = Config::seed,
                             Dimension_Id dimension = Dimension_Id::Overworld);
    ~World_Generator();

    World_Generator(const World_Generator&) = delete;
    World_Generator& operator=(const World_Generator&) = delete;

    void set_generation_folder(const std::string& generation_folder);
    const std::string& get_generation_folder() const { return m_generation_folder; }
    uint32_t get_seed() const { return m_seed; }
    // Измерение (а значит и режим рельефа: Noise / Flat / Void, см. Dimension.h).
    Dimension_Id get_dimension_id() const { return m_dimension->id; }
    const Dimension_Info& get_dimension() const { return *m_dimension; }
    void set_dimension(Dimension_Id dimension) { m_dimension = &get_dimension_info(dimension); }
    // Биом для измерений с single_biome (остальные его игнорируют).
    void set_biome(Biome_Id biome) { m_biome = biome; }
    Biome_Id get_biome() const { return m_biome; }
    // Точка появления игрока: для обычного рельефа — сама колонка (x, z) и её высота.
    // Для измерения без земли — ближайший парящий остров к (x, z); x и z при этом меняются.
    // Возвращает Y верхнего блока, на котором стоять (0 — островов не нашлось).
    int find_spawn_surface(int& x, int& z) const;
    // Ключ хранения чанков этого измерения (для Chunk_Manager::set_world_storage).
    std::string get_storage_key() const { return m_dimension->storage_key(m_generation_folder); }
    static constexpr int sea_level() { return 63; }

    int get_height(int world_x, int world_z) const;
    // Может ли декорирование вообще изменить этот чанк. false — ни одна из колонок, чьи декорации
    // попадают в чанк (с нахлёстом), не доходит до его высот: цикл по ним можно пропустить целиком.
    bool may_decorate_chunk(int chunk_x, int chunk_y, int chunk_z) const;
    Block_Types get_block(int world_x, int world_y, int world_z) const;
    void decorate_column(Chunk& chunk, int world_x, int world_z) const;
    bool is_solid(int world_x, int world_y, int world_z) const;

    // Пакетная генерация целого чанка. Главный источник ускорения: все 2D-поля
    // (климат, рельеф, высота поверхности, биом) считаются ОДИН раз на колонку —
    // 32x32 = 1024 раза вместо 32768, а связность пещер — один раз на ячейку 16^3
    // вместо одного раза на воксель. Результат идентичен поблочному get_block.
    void generate_chunk(Chunk_Block_Grid& blocks,
                        int chunk_x, int chunk_y, int chunk_z) const;
    // Генерирует несколько чанков одного X/Z-столбца одним проходом. Общие 2D-данные
    // (климат, высота, наклон, биом и маркер деревни) вычисляются один раз на весь столбец.
    // Входные Chunk* должны принадлежать одной колонке X/Z; чанки могут отсутствовать по Y.
    void generate_chunk_column(const std::vector<Chunk*>& chunks) const;
    // Prepare shared X/Z-only fields once, then generate individual Y levels concurrently.
    std::shared_ptr<Prepared_Column> prepare_chunk_column(
        int chunk_x, int chunk_z, const std::vector<int>& chunk_ys) const;
    void generate_prepared_chunk(const std::shared_ptr<Prepared_Column>& prepared,
                                 size_t index, Chunk_Block_Grid& output) const;
    // Variant used by the streaming manager: also initialize per-Y metadata while the
    // voxel buffer is already hot, avoiding a second unpack/scan after generation.
    void generate_prepared_chunk(const std::shared_ptr<Prepared_Column>& prepared,
                                 size_t index, Chunk& chunk) const;

    // Быстрая проверка "может ли в этом чанке вообще что-то быть": если весь
    // столбец пород лежит ниже чанка, а вода — тем более, чанк гарантированно
    // пустой и его не надо ни генерировать, ни мешить.
    bool chunk_is_definitely_air(int chunk_x, int chunk_y, int chunk_z) const;

    // Regional cave/hydrology data can take far longer than the per-column voxel fill.
    // Chunk_Manager uses these non-blocking probes to route cold columns through its
    // preparation queue before they enter the latency-sensitive chunk workers.
    static constexpr uint8_t kRegionPartCaves = 1u;
    static constexpr uint8_t kRegionPartRivers = 2u;
    uint8_t generation_region_missing_parts_for_chunk(int chunk_x, int chunk_z) const;
    bool generation_region_ready_for_chunk(int chunk_x, int chunk_z) const;
    void prewarm_generation_region_parts_for_chunk(int chunk_x, int chunk_z, uint8_t parts) const;
    void prewarm_generation_region_for_chunk(int chunk_x, int chunk_z) const;

    World_Sample sample(int world_x, int world_y, int world_z) const;
    World_Sample sample_with_connections(int world_x, int world_y, int world_z,
                                         double water_connection,
                                         double surface_connection) const;
    std::string get_biome_name(int world_x, int world_y, int world_z) const;
    // Биом в точке — тот же, что показывает F3 и что используют спавн мобов.
    Biome_Id biome_id(int world_x, int world_y, int world_z) const;
    bool cave_at(int world_x, int world_y, int world_z) const;

private:
    struct Noise_Field {
        OpenSimplexEnv* environment = nullptr;
        OpenSimplexGradients* gradients = nullptr;

        Noise_Field() = default;
        Noise_Field(OpenSimplexEnv* env, long seed);
        double noise2(double x, double z) const;
        double noise3(double x, double y, double z) const;
    };

    // Всё, что зависит только от (x, z). Считается один раз и переиспользуется
    // для всех блоков колонки.
public:
    // Рельеф одной колонки: то, что нужно и полной Column, и построителям пещер/рек (им — только высота).
    // Публичный только ради потокового кэша в .cpp.
    struct Terrain_Point {
        int key_x = 0, key_z = 0;
        bool valid = false;
        double continentalness = 0.0;
        double erosion = 0.0;
        double mountainness = 0.0;
        double surface_height = 0.0;
        uint32_t epoch = 0;
    };

private:
    struct Subsurface_Coordinates {
        // The same X/Z multiplications are used by every Y in this column. Hoisted into
        // the immutable prepared column to avoid repeating them for every subsurface voxel.
        double ore_x = 0.0;
        double ore_z = 0.0;
    };

    struct Column {
        double temperature = 0.0;
        double humidity = 0.0;
        double continentalness = 0.0;
        double erosion = 0.0;
        double weirdness = 0.0;
        double mountainness = 0.0;
        double depth_parameter = 0.0;
        double surface_height = 0.0;
        double slope = 0.0;
        int    height = 0;            // floor(surface_height)
        Biome_Id biome = Biome_Id::Plains;
        bool   near_ocean = false;
        bool   valid = false;
        uint32_t epoch = 0;
        uint32_t stamp = 0;     // момент последнего использования (LRU в наборе)
        int    key_x = 0, key_z = 0;
    };

    // Внутренний путь генерации: outputs и chunk_ys имеют count элементов и относятся к одной X/Z-колонке.
    void generate_chunks(Chunk_Block_Grid* const* outputs, const int* chunk_ys,
                        size_t count, int chunk_x, int chunk_z) const;
    void generate_prepared_chunk_impl(const std::shared_ptr<Prepared_Column>& prepared,
                                      size_t index, Chunk_Block_Grid& output,
                                      Chunk* owner) const;

    struct Biome_Definition {
        Biome_Id id = Biome_Id::Plains;
        const char* name = "";

        double min_temperature = -1.0, max_temperature = 1.0;
        double min_humidity = -1.0, max_humidity = 1.0;
        double min_continentalness = -1.0, max_continentalness = 1.0;
        double min_erosion = -1.0, max_erosion = 1.0;
        double min_weirdness = -1.0, max_weirdness = 1.0;
        double min_depth = -1.0, max_depth = 1.0;

        double priority = 0.0;

        bool requires_mountainness = false;
        double min_mountainness = 0.0;
        bool surface_candidate = false;   // участвует в обычном поиске по 6 измерениям
    };

    OpenSimplexEnv* m_noise_environment = nullptr;
    Noise_Field m_temperature;
    Noise_Field m_humidity;
    Noise_Field m_continental;
    Noise_Field m_erosion;
    Noise_Field m_weirdness;
    Noise_Field m_terrain;
    Noise_Field m_detail;
    Noise_Field m_mountain;
    Noise_Field m_caves;                       // шум руды (пещеры теперь в Cave_Network)
    std::unique_ptr<Cave_Network> m_cave_network;
    std::unique_ptr<River_Network> m_river_network;
    std::unique_ptr<Structure_Placement> m_structures;

    std::vector<Biome_Definition> m_biomes;
    std::string m_generation_folder;
    const Dimension_Info* m_dimension;
    uint32_t m_seed;
    int m_water_level;
    Biome_Id m_biome = Biome_Id::Plains;
    // Эпоха кэшей: у каждого нового генератора она своя, поэтому thread_local кэши
    // прошлого мира (другой сид/пресет) автоматически считаются промахом.
    uint32_t m_cache_epoch = 0;

    static double clamp01(double value);
    static double smoothstep(double edge0, double edge1, double x);
    static double normalize_noise(double value);
    static double split_signed_noise(double value);
    static double spread_noise(double value);
    static uint64_t splitmix64(uint64_t value);
    static uint32_t derive_seed(uint32_t world_seed, uint64_t salt);

    double fbm2(const Noise_Field& noise, double x, double z,
                int octaves, double frequency, double lacunarity, double gain) const;
    double fbm3(const Noise_Field& noise, double x, double y, double z,
                int octaves, double frequency, double lacunarity, double gain) const;

    // --- Рельеф ---------------------------------------------------------
    // Высота поверхности как НЕПРЕРЫВНАЯ функция климата. Ни одного ветвления
    // вида "if (C < -0.19) обрезать высоту" — именно такое ветвление и срезало
    // горы вертикальной стеной на границе океана.
    double continental_elevation(double continentalness) const;
    double raw_surface_height(int world_x, int world_z,
                              double continentalness, double erosion) const;
    // То же, но с уже посчитанным сигналом гор (normalize_noise(fbm2(m_mountain, ...))): тот же fbm
    // дважды считать незачем.
    double raw_surface_height(int world_x, int world_z,
                              double continentalness, double erosion, double mountain_signal) const;
    // Рельеф колонки через потоковый кэш (без климата, наклона и биома).
    Terrain_Point terrain_point(int world_x, int world_z) const;

    Column build_column(int world_x, int world_z) const;
    Column column_at(int world_x, int world_z) const;

    bool sky_island_block(int world_x, int world_y, int world_z) const;
    bool sky_anchor_valid(int anchor_x, int anchor_z) const;

    void register_default_biomes();
    Biome_Id surface_biome_for_column(const Column& column) const;
    Biome_Id biome_at(const Column& column, int world_y, Cave_Kind cave) const;

    Fast_Rng make_rng(int world_x, int world_z, int salt = 0) const;
    Block_Types surface_block_for(int world_x, int world_z, const Column& column) const;
    Block_Types subsurface_block_for(int world_x, int world_y, int world_z,
                                     const Column& column,
                                     const Subsurface_Coordinates* coordinates = nullptr,
                                     double ore_y = 0.0) const;
    Block_Types block_in_column(const Column& column,
                                int world_x, int world_y, int world_z) const;
    Block_Types block_in_column(const Column& column,
                                int world_x, int world_y, int world_z,
                                bool village_center) const;
    Block_Types block_in_column(const Column& column,
                                int world_x, int world_y, int world_z,
                                bool village_center,
                                const River_Network::Column* river_column,
                                const Cave_Kind* cave_kind,
                                const Subsurface_Coordinates* material_coordinates = nullptr,
                                double ore_y = 0.0) const;
};

#endif // OPTICRAFT_WORLD_GENERATOR_H
