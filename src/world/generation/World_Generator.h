#ifndef OPTICRAFT_WORLD_GENERATOR_H
#define OPTICRAFT_WORLD_GENERATOR_H

#include "world/blocks/Block_Types.h"
#include "world/dimensions/Dimension.h"
#include "utils/Config.h"
#include "OpenSimplex2S.h"
#include "math/Fast_Rng.h"

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
    Block_Types get_block(int world_x, int world_y, int world_z) const;
    void decorate_column(Chunk& chunk, int world_x, int world_z) const;
    bool is_solid(int world_x, int world_y, int world_z) const;

    // Пакетная генерация целого чанка. Главный источник ускорения: все 2D-поля
    // (климат, рельеф, высота поверхности, биом) считаются ОДИН раз на колонку —
    // 32x32 = 1024 раза вместо 32768, а связность пещер — один раз на ячейку 16^3
    // вместо одного раза на воксель. Результат идентичен поблочному get_block.
    void generate_chunk(Chunk_Block_Grid& blocks,
                        int chunk_x, int chunk_y, int chunk_z) const;

    // Быстрая проверка "может ли в этом чанке вообще что-то быть": если весь
    // столбец пород лежит ниже чанка, а вода — тем более, чанк гарантированно
    // пустой и его не надо ни генерировать, ни мешить.
    bool chunk_is_definitely_air(int chunk_x, int chunk_y, int chunk_z) const;

    World_Sample sample(int world_x, int world_y, int world_z) const;
    World_Sample sample_with_connections(int world_x, int world_y, int world_z,
                                         double water_connection,
                                         double surface_connection) const;
    std::string get_biome_name(int world_x, int world_y, int world_z) const;
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
    Noise_Field m_caves;

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

    Column build_column(int world_x, int world_z) const;
    Column column_at(int world_x, int world_z) const;

    // Поля пещер хранятся в узлах решёток и интерполируются трилинейно: тоннели — на шаге 2,
    // залы — на шаге 4 (они низкочастотные). Узел — чистая функция своих мировых координат,
    // поэтому любой чанк и декорация получают одно и то же значение в одной точке мира.
    float compute_lattice_value(bool chambers, int channel,
                                int node_x, int node_y, int node_z) const;
    float cave_lattice_value(bool chambers, int channel,
                             int node_x, int node_y, int node_z) const;
    void begin_cave_window(int world_x, int world_y, int world_z) const;
    double lattice_sum(bool chambers, int channel, int base_x, int base_y, int base_z,
                       double fx, double fy, double fz) const;

    double cave_density_at(int world_x, int world_y, int world_z,
                           double surface_height) const;
    // Плотное поле одного чанка: тоннели/залы в массивах, без проверок окна (см. generate_chunk).
    struct Cave_Field;
    void begin_cave_field(Cave_Field& field, int world_x, int world_y, int world_z,
                          int top_y) const;
    double cave_density_dense(Cave_Field& field, int world_x, int world_y, int world_z,
                              double surface_height) const;
    bool sky_island_block(int world_x, int world_y, int world_z) const;
    bool sky_anchor_valid(int anchor_x, int anchor_z) const;

    // Связность пещер считается на сетке 16^3 и кэшируется: биому пещеры не нужна
    // поблочная точность, а прежний поиск в ширину на КАЖДЫЙ воксель стоил
    // десятки секунд на один подземный чанк.
    struct Cave_Flags { bool surface_connected = false; bool water_connected = false; };
    Cave_Flags cave_flags_for_cell(int cell_x, int cell_y, int cell_z) const;
    Cave_Flags cave_flags_at(int world_x, int world_y, int world_z) const;
    bool cave_cell_open(int world_x, int world_y, int world_z) const;

    void register_default_biomes();
    Biome_Id surface_biome_for_column(const Column& column) const;
    Biome_Id biome_at(const Column& column, int world_y,
                      double cave_density, const Cave_Flags& flags) const;

    Fast_Rng make_rng(int world_x, int world_z, int salt = 0) const;
    Block_Types surface_block_for(int world_x, int world_z, const Column& column) const;
    Block_Types subsurface_block_for(int world_x, int world_y, int world_z,
                                     const Column& column) const;
    Block_Types block_in_column(const Column& column,
                                int world_x, int world_y, int world_z,
                                Cave_Field* field = nullptr) const;
};

#endif // OPTICRAFT_WORLD_GENERATOR_H
