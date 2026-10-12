#include "world/generation/World_Generator.h"

#include "world/chunks/Chunk.h"
#include "world/generation/Tree_Generator.h"
#include "utils/logging/Logger.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace {
    // Уровень моря. Вода занимает Y <= 63 везде, где поверхность ниже этого уровня.
    constexpr int kSeaLevel = 63;

    // Порог пещер общий для всех решений о пещерах/биомах.
    constexpr double kCaveThreshold = 0.58;

    // Пороговые высоты горных биомов (абсолютные, над Y=0). Суша обычно лежит в 66..100.
    constexpr double kMountainHeight = 118.0;
    constexpr double kHighMountainHeight = 175.0;
    constexpr double kPlateauHeight = 92.0;

    // Размер ячейки, на которой считается связность пещер (см. cave_flags_for_cell).
    constexpr int kCaveCellSize = 16;

    uint64_t hash_position(int x, int z, uint32_t seed, int salt) {
        uint64_t h = static_cast<uint64_t>(seed);
        h ^= static_cast<uint64_t>(static_cast<int64_t>(x)) * 0x9E3779B185EBCA87ull;
        h ^= static_cast<uint64_t>(static_cast<int64_t>(z)) * 0xC2B2AE3D27D4EB4Full;
        h ^= static_cast<uint64_t>(static_cast<int64_t>(salt)) * 0x165667B19E3779F9ull;
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 32;
        return h;
    }

    uint64_t hash_position3(int x, int y, int z, uint32_t seed, int salt) {
        uint64_t h = hash_position(x, z, seed, salt);
        h ^= static_cast<uint64_t>(static_cast<int64_t>(y)) * 0xD6E8FEB86659FD93ull;
        h ^= h >> 31;
        h *= 0x94D049BB133111EBull;
        return h ^ (h >> 33);
    }

    // Дешёвый детерминированный "бросок кубика" 0..99 вместо конструирования
    // std::mt19937 (624 слова состояния) на каждый блок поверхности.
    int deterministic_roll(int x, int z, uint32_t seed, int salt, int modulo) {
        return static_cast<int>(hash_position(x, z, seed, salt) % static_cast<uint64_t>(modulo));
    }

    double interval_distance(double value, double min_value, double max_value) {
        if (value < min_value) return min_value - value;
        if (value > max_value) return value - max_value;
        return 0.0;
    }

    double sq(double v) { return v * v; }

    int floor_div(int value, int divisor) {
        const int q = value / divisor;
        return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? q - 1 : q;
    }

    const char* const kBiomeNames[] = {
        "Ocean", "Shallow Water", "Beach", "Plains", "Forest", "Birch Forest",
        "Taiga", "Jungle", "Savanna", "Desert", "Stone Desert", "Steppe",
        "Tundra", "Ice Fields", "Mountains", "High Mountains", "Plateau",
        "Slime", "Solar", "Sky", "Underground", "Deep Underground",
        "Flooded Underground"
    };
    static_assert(sizeof(kBiomeNames) / sizeof(kBiomeNames[0]) ==
                  static_cast<size_t>(Biome_Id::Count),
                  "Biome name table must cover every Biome_Id");
}

const char* biome_name(Biome_Id id) {
    const auto index = static_cast<size_t>(id);
    return index < static_cast<size_t>(Biome_Id::Count) ? kBiomeNames[index] : "Plains";
}

// ---------------------------------------------------------------------------
//  Потоковые кэши
// ---------------------------------------------------------------------------
// Все три кэша — thread_local, поэтому воркеры Chunk_Manager не делят между собой
// ни одной строки памяти и не нуждаются в синхронизации. Каждый кэш прямого
// отображения (direct-mapped): попадание — одно сравнение ключа, промах — пересчёт.
namespace {
    struct Terrain_Point {
        int key_x = 0, key_z = 0;
        bool valid = false;
        double continentalness = 0.0;
        double erosion = 0.0;
        double mountainness = 0.0;
        double surface_height = 0.0;
        uint32_t epoch = 0;
    };

    constexpr size_t kTerrainCacheBits = 13;             // 8192 колонок
    constexpr size_t kTerrainCacheSize = 1u << kTerrainCacheBits;
    constexpr size_t kTerrainCacheMask = kTerrainCacheSize - 1;

    constexpr size_t kColumnCacheBits = 12;              // 4096 колонок
    constexpr size_t kColumnCacheSize = 1u << kColumnCacheBits;
    constexpr size_t kColumnCacheMask = kColumnCacheSize - 1;

    constexpr size_t kCaveCellCacheBits = 11;            // 2048 ячеек 16^3
    constexpr size_t kCaveCellCacheSize = 1u << kCaveCellCacheBits;
    constexpr size_t kCaveCellCacheMask = kCaveCellCacheSize - 1;

    size_t cache_index2(int x, int z, size_t mask) {
        uint64_t h = static_cast<uint64_t>(static_cast<int64_t>(x)) * 0x9E3779B185EBCA87ull ^
                     static_cast<uint64_t>(static_cast<int64_t>(z)) * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 31;
        return static_cast<size_t>(h) & mask;
    }

    size_t cache_index3(int x, int y, int z, size_t mask) {
        uint64_t h = static_cast<uint64_t>(static_cast<int64_t>(x)) * 0x9E3779B185EBCA87ull ^
                     static_cast<uint64_t>(static_cast<int64_t>(y)) * 0xD6E8FEB86659FD93ull ^
                     static_cast<uint64_t>(static_cast<int64_t>(z)) * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 31;
        return static_cast<size_t>(h) & mask;
    }
}

World_Generator::Noise_Field::Noise_Field(OpenSimplexEnv* env, long seed)
    : environment(env)
    , gradients(newOpenSimplexGradients(env, seed))
{}

double World_Generator::Noise_Field::noise2(double x, double z) const {
    return ::noise2(environment, gradients, x, z);
}

double World_Generator::Noise_Field::noise3(double x, double y, double z) const {
    return ::noise3_XZBeforeY(environment, gradients, x, y, z);
}

World_Generator::World_Generator(const std::string& generation_folder, uint32_t seed)
    : m_generation_folder(generation_folder)
    , m_seed(seed)
    , m_water_level(kSeaLevel)
{
    // Каждый экземпляр получает свою эпоху, чтобы thread_local кэши не отдавали
    // данные предыдущего мира после смены сида или пресета генерации.
    static std::atomic<uint32_t> s_epoch_counter{1};
    m_cache_epoch = s_epoch_counter.fetch_add(1, std::memory_order_relaxed);

    m_noise_environment = initOpenSimplex();
    if (!m_noise_environment) {
        LOG_ERROR("World_Generator: failed to initialize OpenSimplex2S");
        return;
    }

    m_temperature = Noise_Field(m_noise_environment, derive_seed(seed, 0x01));
    m_humidity = Noise_Field(m_noise_environment, derive_seed(seed, 0x02));
    m_continental = Noise_Field(m_noise_environment, derive_seed(seed, 0x03));
    m_erosion = Noise_Field(m_noise_environment, derive_seed(seed, 0x04));
    m_weirdness = Noise_Field(m_noise_environment, derive_seed(seed, 0x05));
    m_terrain = Noise_Field(m_noise_environment, derive_seed(seed, 0x06));
    m_detail = Noise_Field(m_noise_environment, derive_seed(seed, 0x07));
    m_mountain = Noise_Field(m_noise_environment, derive_seed(seed, 0x08));
    m_caves = Noise_Field(m_noise_environment, derive_seed(seed, 0x09));

    register_default_biomes();
}

World_Generator::~World_Generator() = default;

void World_Generator::set_generation_folder(const std::string& generation_folder) {
    m_generation_folder = generation_folder;
}

double World_Generator::clamp01(double value) { return std::clamp(value, 0.0, 1.0); }

double World_Generator::smoothstep(double edge0, double edge1, double x) {
    const double t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

double World_Generator::normalize_noise(double value) { return clamp01(value * 0.5 + 0.5); }

double World_Generator::split_signed_noise(double value) { return std::clamp(value, -1.0, 1.0); }

// fbm — это среднее нескольких октав шума, поэтому его значения кучкуются у 0.5:
// измеренно 90% колонок суши имеют температуру/влажность в диапазоне 0.29..0.71
// (sigma ~ 0.14). Таблица биомов же рассчитана на весь диапазон 0..1 (пустыня — это
// T=0.9 / H=0.1, ледяные равнины — T=0.05), и такие биомы не выпадали вообще.
// Функция распределения нормального закона превращает «колокол» в почти равномерное
// распределение на [0, 1], так что каждый угол таблицы T/H получает свою долю карты.
double World_Generator::spread_noise(double value) {
    constexpr double kSigma = 0.14;
    return clamp01(0.5 * (1.0 + std::erf((value - 0.5) / (kSigma * 1.41421356237))));
}

uint64_t World_Generator::splitmix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

uint32_t World_Generator::derive_seed(uint32_t world_seed, uint64_t salt) {
    return static_cast<uint32_t>(splitmix64(static_cast<uint64_t>(world_seed) ^ salt) ^
                                 (splitmix64(static_cast<uint64_t>(world_seed) ^ salt) >> 32));
}

double World_Generator::fbm2(const Noise_Field& noise, double x, double z,
                             int octaves, double frequency,
                             double lacunarity, double gain) const {
    double sum = 0.0;
    double amplitude = 1.0;
    double amplitude_sum = 0.0;

    for (int i = 0; i < octaves; ++i) {
        sum += noise.noise2(x * frequency, z * frequency) * amplitude;
        amplitude_sum += amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }
    return amplitude_sum > 0.0 ? sum / amplitude_sum : 0.0;
}

double World_Generator::fbm3(const Noise_Field& noise, double x, double y, double z,
                             int octaves, double frequency,
                             double lacunarity, double gain) const {
    double sum = 0.0;
    double amplitude = 1.0;
    double amplitude_sum = 0.0;

    for (int i = 0; i < octaves; ++i) {
        sum += noise.noise3(x * frequency, y * frequency, z * frequency) * amplitude;
        amplitude_sum += amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }
    return amplitude_sum > 0.0 ? sum / amplitude_sum : 0.0;
}

// ---------------------------------------------------------------------------
//  ГЛАВНОЕ ИСПРАВЛЕНИЕ: непрерывный профиль берега
// ---------------------------------------------------------------------------
// Раньше высота собиралась в три приёма: сначала "естественный" рельеф, потом
// притягивание низкого побережья к уровню моря маской, которая сама зависела от
// уже посчитанной высоты, и наконец жёсткое
//
//     if (C < -0.19) surface = min(surface, 63 - depth);
//
// Последняя строка и резала горы: при C = -0.1899 гора высотой 153 оставалась
// нетронутой, при C = -0.1901 её целиком срезало до 62. Отсюда вертикальная
// стена в 91 блок на расстоянии двух блоков — ровно то, что видно на скриншоте.
//
// Теперь высота — одна непрерывная функция климата. Океан задаётся не обрезанием
// готовой высоты, а сплайном базовой отметки, а горный член умножается на
// непрерывную маску суши. Разрыв стал невозможен по построению.
double World_Generator::continental_elevation(double c) const {
    // Монотонный сплайн "континентальность -> базовая отметка".
    struct Knot { double c; double elevation; };
    static constexpr Knot kKnots[] = {
        {-1.00, kSeaLevel - 46.0},   // глубокий океан
        {-0.62, kSeaLevel - 30.0},   // океан
        {-0.34, kSeaLevel - 13.0},   // шельф
        {-0.17, kSeaLevel -  3.5},   // подводный склон пляжа
        {-0.06, kSeaLevel +  2.0},   // линия прибоя
        { 0.10, kSeaLevel +  8.0},   // прибрежная равнина
        { 0.38, kSeaLevel + 18.0},
        { 0.70, kSeaLevel + 30.0},
        { 1.00, kSeaLevel + 44.0}
    };
    constexpr int kKnotCount = sizeof(kKnots) / sizeof(kKnots[0]);

    if (c <= kKnots[0].c) return kKnots[0].elevation;
    for (int i = 1; i < kKnotCount; ++i) {
        if (c <= kKnots[i].c) {
            // smoothstep между узлами: значение и первая производная непрерывны,
            // поэтому на стыках сегментов не возникает ни ступеньки, ни излома.
            const double t = smoothstep(kKnots[i - 1].c, kKnots[i].c, c);
            return kKnots[i - 1].elevation +
                   (kKnots[i].elevation - kKnots[i - 1].elevation) * t;
        }
    }
    return kKnots[kKnotCount - 1].elevation;
}

double World_Generator::raw_surface_height(int world_x, int world_z,
                                           double continentalness,
                                           double erosion) const {
    const double broad_terrain = fbm2(m_terrain, world_x, world_z, 5, 0.0022, 2.0, 0.5);
    const double detail_terrain = fbm2(m_detail, world_x, world_z, 3, 0.012, 2.0, 0.5);
    const double mountain_signal = normalize_noise(
        fbm2(m_mountain, world_x, world_z, 4, 0.00125, 2.0, 0.5));

    const double base = continental_elevation(continentalness);

    // Маска суши: 0 в открытом океане, 1 вглубь материка. Через неё проходит ВЕСЬ
    // рельеф, поэтому горы плавно уходят под воду вместо того, чтобы обрываться.
    const double land = smoothstep(-0.32, 0.02, continentalness);

    // Низкая эрозия = изрезанный рельеф, высокая = сглаженный.
    const double ruggedness = 1.0 - smoothstep(0.35, 0.85, erosion);

    // Гашение рельефа у самой воды: около уровня моря амплитуда падает, из-за чего
    // берег выходит пологим пляжем, а не обрывом. Функция гладкая (гауссиана),
    // так что никакого разрыва не добавляет.
    const double distance_to_waterline = (base - kSeaLevel) / 11.0;
    const double shore_damp = 0.30 + 0.70 * clamp01(
        1.0 - std::exp(-distance_to_waterline * distance_to_waterline));

    const double hill_amplitude = (9.0 + ruggedness * 34.0) *
                                  (0.30 + 0.70 * land) * shore_damp;

    // Горный член тоже умножен на land и на shore_damp — ни один "срез" его больше
    // не трогает, гора просто не успевает вырасти у самой кромки воды.
    const double mountain_gate = smoothstep(0.50, 0.80, mountain_signal) *
                                 ruggedness * land * shore_damp;

    const double detail_amplitude = (2.5 + ruggedness * 5.5) * (0.35 + 0.65 * land);

    const double height = base
                        + broad_terrain * hill_amplitude
                        + mountain_gate * 235.0
                        + detail_terrain * detail_amplitude;

    return std::clamp(height,
                      static_cast<double>(Config::world_min_y + 1),
                      static_cast<double>(Config::world_max_y));
}

double World_Generator::cave_density_at(int world_x, int world_y, int world_z,
                                        double surface_height) const {
    if (world_y > surface_height - 4.0) return 0.0;

    // Шум пещер не менялся: те же три FBM-поля и те же параметры. Изменился только
    // порядок вычислений — добавлены ранние выходы, которые НЕ меняют результат,
    // но позволяют не считать второе и третье поле там, где они уже ни на что
    // не влияют. На типичном подземном чанке это убирает около 60% выборок шума.
    constexpr double kTunnelBand = 0.16;

    const double depth = clamp01(
        (surface_height - static_cast<double>(world_y) - 80.0) / 420.0);

    const double n1 = fbm3(m_caves, world_x, world_y * 1.4, world_z,
                           3, 0.018, 2.0, 0.5);

    double tunnel = 0.0;
    if (std::abs(n1) < kTunnelBand) {
        // band1 > 0 — только тогда второе поле вообще может дать ненулевой тоннель.
        const double n2 = fbm3(m_caves, world_x + 4096.0, world_y * 1.4,
                               world_z - 4096.0, 3, 0.018, 2.0, 0.5);
        const double band1 = 1.0 - std::abs(n1) / kTunnelBand;
        const double band2 = clamp01(1.0 - std::abs(n2) / kTunnelBand);
        tunnel = band1 * band2;
    }

    // Залы дают вклад только глубже 80 блоков под поверхностью (иначе depth = 0)
    // и только если тоннель ещё не насытил результат.
    if (depth <= 0.0 || tunnel >= 1.0) return clamp01(tunnel);

    const double chambers = normalize_noise(fbm3(
        m_caves, world_x + 1731.0, world_y * 0.6, world_z - 941.0,
        3, 0.0055, 2.0, 0.55));

    const double cavern = chambers > 0.85 ? (chambers - 0.85) / 0.15 : 0.0;
    const double cavern_signal = cavern * depth;

    return clamp01(std::max(tunnel, cavern_signal));
}

// ---------------------------------------------------------------------------
//  Кэш рельефа и колонок
// ---------------------------------------------------------------------------
namespace {
    Terrain_Point* terrain_cache_slot(int x, int z) {
        static thread_local Terrain_Point cache[kTerrainCacheSize];
        return &cache[cache_index2(x, z, kTerrainCacheMask)];
    }
}

// Континентальность, эрозия, горность и высота поверхности одной колонки.
// Кэшируется отдельно от полной Column, потому что эти же данные нужны соседним
// колонкам (для наклона) и поиску связности пещер.
World_Generator::Column World_Generator::build_column(int world_x, int world_z) const {
    Column column;
    column.key_x = world_x;
    column.key_z = world_z;
    column.valid = true;

    if (!m_noise_environment) return column;

    const auto terrain_of = [this](int x, int z) -> Terrain_Point {
        Terrain_Point* slot = terrain_cache_slot(x, z);
        if (slot->valid && slot->epoch == m_cache_epoch &&
            slot->key_x == x && slot->key_z == z) {
            return *slot;
        }

        Terrain_Point point;
        point.key_x = x;
        point.key_z = z;
        point.valid = true;
        point.epoch = m_cache_epoch;
        point.continentalness = split_signed_noise(
            fbm2(m_continental, x, z, 5, 0.00022, 2.0, 0.5));
        point.erosion = normalize_noise(fbm2(m_erosion, x, z, 4, 0.0018, 2.0, 0.5));
        point.mountainness = normalize_noise(
            fbm2(m_mountain, x, z, 4, 0.00125, 2.0, 0.5));
        point.surface_height = raw_surface_height(x, z,
                                                  point.continentalness,
                                                  point.erosion);
        *slot = point;
        return point;
    };

    const Terrain_Point here = terrain_of(world_x, world_z);
    column.continentalness = here.continentalness;
    column.erosion = here.erosion;
    column.mountainness = here.mountainness;
    column.surface_height = here.surface_height;
    column.height = static_cast<int>(std::floor(here.surface_height));

    // Наклон нужен, чтобы песок ложился только на пологий берег, а не на отвесную
    // скалу. Соседние точки рельефа почти всегда уже в кэше — их считает соседняя
    // колонка того же чанка.
    const double east = terrain_of(world_x + 1, world_z).surface_height;
    const double north = terrain_of(world_x, world_z + 1).surface_height;
    column.slope = std::max(std::abs(east - here.surface_height),
                            std::abs(north - here.surface_height));

    column.temperature = spread_noise(normalize_noise(
        fbm2(m_temperature, world_x, world_z, 4, 0.00055, 2.0, 0.5)));
    column.humidity = spread_noise(normalize_noise(
        fbm2(m_humidity, world_x, world_z, 4, 0.00075, 2.0, 0.5)));
    column.weirdness = normalize_noise(
        fbm2(m_weirdness, world_x, world_z, 3, 0.0009, 2.0, 0.5));

    // Depth — детерминированный сплайн континентальности (не отдельный шум).
    const double c = column.continentalness;
    if (c <= -0.45) {
        column.depth_parameter = -1.0 + smoothstep(-1.0, -0.45, c) * 0.20;
    } else if (c < -0.19) {
        column.depth_parameter = -0.80 + smoothstep(-0.45, -0.19, c) * 0.60;
    } else if (c < 0.30) {
        column.depth_parameter = -0.20 + smoothstep(-0.19, 0.30, c) * 0.35;
    } else if (c < 0.70) {
        column.depth_parameter = 0.15 + smoothstep(0.30, 0.70, c) * 0.45;
    } else {
        column.depth_parameter = 0.60 + smoothstep(0.70, 1.0, c) * 0.40;
    }

    // "Рядом океан" теперь определяется по фактической высоте соседей, а не по
    // 25 дополнительным выборкам шума на каждую колонку.
    column.near_ocean = column.surface_height < kSeaLevel + 6.0 ||
                        column.continentalness < -0.14;

    column.biome = surface_biome_for_column(column);
    return column;
}

World_Generator::Column World_Generator::column_at(int world_x, int world_z) const {
    static thread_local Column cache[kColumnCacheSize];
    Column& slot = cache[cache_index2(world_x, world_z, kColumnCacheMask)];
    if (slot.valid && slot.key_x == world_x && slot.key_z == world_z &&
        slot.epoch == m_cache_epoch) {
        return slot;
    }
    Column built = build_column(world_x, world_z);
    built.epoch = m_cache_epoch;
    slot = built;
    return built;
}

// ---------------------------------------------------------------------------
//  Связность пещер (на сетке 16^3, с кэшем)
// ---------------------------------------------------------------------------
// Прежняя версия запускала поиск в ширину на 4096 узлов ДЛЯ КАЖДОГО ВОКСЕЛЯ, и
// каждый узел заново считал полный набор шумов. Замер: один подземный чанк
// генерировался 33 секунды. Биому пещеры поблочная точность не нужна — флаги
// считаются один раз на ячейку 16x16x16 и кладутся в кэш, а сам обход остаётся
// честным поиском компоненты связности, просто с шагом 2 блока и жёстким лимитом.
bool World_Generator::cave_cell_open(int world_x, int world_y, int world_z) const {
    if (world_y < Config::world_min_y || world_y > Config::world_max_y) return false;
    const double surface = column_at(world_x, world_z).surface_height;
    if (world_y >= surface - 4.0) return false;
    return cave_density_at(world_x, world_y, world_z, surface) > kCaveThreshold;
}

World_Generator::Cave_Flags
World_Generator::cave_flags_for_cell(int cell_x, int cell_y, int cell_z) const {
    Cave_Flags flags;

    const int origin_x = cell_x * kCaveCellSize + kCaveCellSize / 2;
    const int origin_y = cell_y * kCaveCellSize + kCaveCellSize / 2;
    const int origin_z = cell_z * kCaveCellSize + kCaveCellSize / 2;

    if (!cave_cell_open(origin_x, origin_y, origin_z)) return flags;

    constexpr int kStep = 2;              // шаг обхода, блоки
    constexpr int kMaxNodes = 320;        // жёсткий потолок работы
    constexpr int kMaxHorizontal = 96;
    constexpr int kMaxVertical = 128;

    struct Node { int x, y, z; };
    Node queue[kMaxNodes];
    int head = 0, tail = 0;

    // Посещённые узлы — открытая адресация в таблице фиксированного размера
    // (линейный поиск по массиву давал бы O(n^2) на каждую ячейку).
    constexpr size_t kVisitedSize = 1024;   // > 2 * kMaxNodes, степень двойки
    uint64_t visited_keys[kVisitedSize] = {};

    const auto try_mark = [&](int x, int y, int z) -> bool {
        const uint64_t key = (hash_position3(x, y, z, m_seed, 0x7E5) | 1ull);
        size_t index = static_cast<size_t>(key) & (kVisitedSize - 1);
        for (size_t probe = 0; probe < kVisitedSize; ++probe) {
            if (visited_keys[index] == 0) { visited_keys[index] = key; return true; }
            if (visited_keys[index] == key) return false;   // уже были
            index = (index + 1) & (kVisitedSize - 1);
        }
        return false;
    };

    queue[tail++] = {origin_x, origin_y, origin_z};
    try_mark(origin_x, origin_y, origin_z);

    while (head < tail && tail < kMaxNodes) {
        const Node n = queue[head++];

        const Column here = column_at(n.x, n.z);
        // Компонента дошла до поверхности.
        if (n.y >= static_cast<int>(std::floor(here.surface_height)) - 6) {
            flags.surface_connected = true;
        }
        // Компонента дошла до воды: клетка ниже уровня моря, а поверхность этой
        // колонки ещё ниже, значит над ней стоит настоящая вода.
        if (n.y <= kSeaLevel && here.surface_height < n.y) {
            flags.water_connected = true;
        }
        if (flags.surface_connected && flags.water_connected) break;

        static constexpr int dirs[6][3] = {
            {kStep,0,0},{-kStep,0,0},{0,kStep,0},{0,-kStep,0},{0,0,kStep},{0,0,-kStep}
        };
        for (const auto& d : dirs) {
            const int nx = n.x + d[0], ny = n.y + d[1], nz = n.z + d[2];
            if (std::abs(nx - origin_x) > kMaxHorizontal ||
                std::abs(nz - origin_z) > kMaxHorizontal ||
                std::abs(ny - origin_y) > kMaxVertical) continue;
            if (!try_mark(nx, ny, nz)) continue;
            if (tail < kMaxNodes && cave_cell_open(nx, ny, nz))
                queue[tail++] = {nx, ny, nz};
        }
    }

    return flags;
}

World_Generator::Cave_Flags
World_Generator::cave_flags_at(int world_x, int world_y, int world_z) const {
    struct Entry {
        int cx = 0, cy = 0, cz = 0;
        uint32_t epoch = 0;
        bool valid = false;
        Cave_Flags flags;
    };
    static thread_local Entry cache[kCaveCellCacheSize];

    const int cx = floor_div(world_x, kCaveCellSize);
    const int cy = floor_div(world_y, kCaveCellSize);
    const int cz = floor_div(world_z, kCaveCellSize);

    Entry& slot = cache[cache_index3(cx, cy, cz, kCaveCellCacheMask)];
    if (slot.valid && slot.epoch == m_cache_epoch &&
        slot.cx == cx && slot.cy == cy && slot.cz == cz) {
        return slot.flags;
    }

    slot.cx = cx; slot.cy = cy; slot.cz = cz;
    slot.epoch = m_cache_epoch;
    slot.valid = true;
    slot.flags = cave_flags_for_cell(cx, cy, cz);
    return slot.flags;
}

// ---------------------------------------------------------------------------
//  Парящие острова
// ---------------------------------------------------------------------------
bool World_Generator::sky_anchor_valid(int anchor_x, int anchor_z) const {
    const double c = split_signed_noise(
        fbm2(m_continental, anchor_x, anchor_z, 5, 0.00022, 2.0, 0.5));
    const double erosion = normalize_noise(
        fbm2(m_erosion, anchor_x, anchor_z, 4, 0.0018, 2.0, 0.5));
    const double mountain_signal = normalize_noise(
        fbm2(m_mountain, anchor_x, anchor_z, 4, 0.00125, 2.0, 0.5));

    const double surface = raw_surface_height(anchor_x, anchor_z, c, erosion);

    const bool mountain = mountain_signal > 0.75 && erosion > 0.25 && surface > 80.0;
    const bool ocean = c < -0.19;
    return mountain || ocean;
}

bool World_Generator::sky_island_block(int world_x, int world_y, int world_z) const {
    if (world_y < 200 || world_y > Config::world_max_y) return false;

    constexpr int kCellSize = 384;

    // Кластер островов зависит только от (x, z), поэтому его параметры считаются
    // один раз на ячейку 384x384 и кэшируются — раньше весь этот поиск по 3x3
    // ячейкам с четырьмя FBM в каждой выполнялся на КАЖДЫЙ воксель выше Y=200.
    struct Cluster {
        int key_x = 0, key_z = 0;
        uint32_t epoch = 0;
        bool valid = false;
        bool anchor_valid = false;
        int center_x = 0, center_z = 0, center_y = 0;
        int main_radius = 0;
    };
    static thread_local Cluster cache[512];

    const int cell_x = floor_div(world_x, kCellSize);
    const int cell_z = floor_div(world_z, kCellSize);

    Cluster& slot = cache[cache_index2(cell_x, cell_z, 511)];
    if (!(slot.valid && slot.epoch == m_cache_epoch &&
          slot.key_x == cell_x && slot.key_z == cell_z)) {
        Cluster cluster;
        cluster.key_x = cell_x;
        cluster.key_z = cell_z;
        cluster.epoch = m_cache_epoch;
        cluster.valid = true;

        double best_distance = std::numeric_limits<double>::max();
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int gx = cell_x + dx;
                const int gz = cell_z + dz;
                const uint64_t h = hash_position(gx, gz, m_seed, 0x51A7);

                const int ox = 48 + static_cast<int>(h & 0xFFu);
                const int oz = 48 + static_cast<int>((h >> 8) & 0xFFu);
                const int cx = gx * kCellSize + ox;
                const int cz = gz * kCellSize + oz;

                const double ddx = static_cast<double>(world_x - cx);
                const double ddz = static_cast<double>(world_z - cz);
                const double distance = ddx * ddx + ddz * ddz;
                if (distance >= best_distance) continue;

                best_distance = distance;
                cluster.center_x = cx;
                cluster.center_z = cz;
                cluster.anchor_valid = sky_anchor_valid(cx, cz);
                cluster.main_radius = 80 + static_cast<int>((h >> 16) % 96u);

                const double c = split_signed_noise(
                    fbm2(m_continental, cx, cz, 5, 0.00022, 2.0, 0.5));
                const double erosion = normalize_noise(
                    fbm2(m_erosion, cx, cz, 4, 0.0018, 2.0, 0.5));
                const double peak = raw_surface_height(cx, cz, c, erosion);

                if (c < -0.19) {
                    cluster.center_y = kSeaLevel + 70 +
                                       static_cast<int>((h >> 24) % 9u) - 4;
                } else {
                    cluster.center_y = static_cast<int>(std::round(peak)) + 100 +
                                       static_cast<int>((h >> 24) % 51u);
                }
            }
        }
        slot = cluster;
    }

    if (!slot.anchor_valid) return false;

    constexpr double kClusterRadius = 375.0;
    const double anchor_dx = static_cast<double>(world_x - slot.center_x);
    const double anchor_dz = static_cast<double>(world_z - slot.center_z);
    const double main_dist = std::sqrt(anchor_dx * anchor_dx + anchor_dz * anchor_dz);
    if (main_dist > kClusterRadius) return false;

    const double main_radius = static_cast<double>(slot.main_radius);
    if (main_dist <= main_radius) {
        const double n = normalize_noise(
            fbm2(m_detail, world_x + slot.center_x * 0.37,
                 world_z + slot.center_z * 0.61, 2, 0.055, 2.0, 0.5));
        const double r = main_radius * (0.86 + n * 0.20);
        if (main_dist <= r) {
            const double q = main_dist / std::max(1.0, r);
            const double top = slot.center_y + 6.0 * (1.0 - q * q);
            const double bottom = slot.center_y -
                                  (8.0 + 58.0 * std::pow(1.0 - q, 1.55));
            if (world_y >= bottom && world_y <= top) return true;
        }
    }

    for (int i = 0; i < 18; ++i) {
        const uint64_t h = hash_position(
            slot.center_x + i * 17, slot.center_z - i * 31, m_seed, 0xA11CE);
        const double angle = (static_cast<double>(h & 0xFFFFu) / 65535.0) * 6.28318530718;
        const double radial = 95.0 + static_cast<double>((h >> 16) % 255u);
        const double sx = static_cast<double>(slot.center_x) + std::cos(angle) * radial;
        const double sz = static_cast<double>(slot.center_z) + std::sin(angle) * radial;

        const double ddx = world_x - sx;
        const double ddz = world_z - sz;
        const double horizontal = std::sqrt(ddx * ddx + ddz * ddz);
        const double radius = 10.0 + static_cast<double>((h >> 32) % 42u);
        if (horizontal > radius) continue;

        const int sy = slot.center_y + static_cast<int>((h >> 40) % 31u) - 15;
        const double q = horizontal / std::max(1.0, radius);
        const double top = sy + 3.0 * (1.0 - q * q);
        const double bottom = sy - (4.0 + 22.0 * std::pow(1.0 - q, 1.5));
        if (world_y >= bottom && world_y <= top) return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
//  Биомы
// ---------------------------------------------------------------------------
void World_Generator::register_default_biomes() {
    m_biomes.clear();

    auto add = [this](Biome_Id id,
                      double t0, double t1, double h0, double h1,
                      double c0, double c1, double e0, double e1,
                      double w0, double w1, double d0, double d1,
                      double priority, bool surface_candidate) {
        Biome_Definition b;
        b.id = id;
        b.name = biome_name(id);
        b.min_temperature = t0; b.max_temperature = t1;
        b.min_humidity = h0; b.max_humidity = h1;
        b.min_continentalness = c0; b.max_continentalness = c1;
        b.min_erosion = e0; b.max_erosion = e1;
        b.min_weirdness = w0; b.max_weirdness = w1;
        b.min_depth = d0; b.max_depth = d1;
        b.priority = priority;
        b.surface_candidate = surface_candidate;
        m_biomes.push_back(b);
    };

    // Океан/мелководье/пляж выбираются по фактической высоте относительно моря (см.
    // surface_biome_for_column), горные биомы — по рельефу. Остальные суша-биомы — это
    // «точки» на плоскости температура/влажность (обе величины растянуты до равномерных
    // 0..1, см. spread_noise), побеждает ближайшая. Раньше в поиск входили ещё
    // континентальность и эрозия с узкими интервалами — в сумме это делало Desert,
    // Ice Fields, Stone Desert и Jungle практически недостижимыми.
    //
    //                          T0   T1   H0   H1   C0   C1   E0   E1   W0   W1   D0  D1  prio
    add(Biome_Id::Ice_Fields,    0.02,0.02, 0.67,0.67, -1,1, -1,1,  0,1, -1,1, 40, true);
    add(Biome_Id::Tundra,        0.12,0.12, 0.05,0.05, -1,1, -1,1,  0,1, -1,1, 60, true);
    add(Biome_Id::Taiga,         0.12,0.12, 0.67,0.67, -1,1, -1,1,  0,1, -1,1, 60, true);
    add(Biome_Id::Plains,        0.30,0.30, 0.27,0.27, -1,1, -1,1,  0,1, -1,1, 50, true);
    add(Biome_Id::Forest,        0.37,0.37, 0.83,0.83, -1,1, -1,1,  0,1, -1,1, 50, true);
    add(Biome_Id::Birch_Forest,  0.70,0.70, 0.75,0.75, -1,1, -1,1,  0,1, -1,1, 45, true);
    add(Biome_Id::Steppe,        0.80,0.80, 0.30,0.30, -1,1, -1,1,  0,1, -1,1, 55, true);
    add(Biome_Id::Savanna,       0.80,0.80, 0.60,0.60, -1,1, -1,1,  0,1, -1,1, 40, true);
    add(Biome_Id::Jungle,        0.76,0.76, 0.86,0.86, -1,1, -1,1,  0,1, -1,1, 35, true);
    // Две пустыни делят один угол T/H и различаются «странностью» (weirdness).
    add(Biome_Id::Desert,        0.85,0.85, 0.20,0.20, -1,1, -1,1,  0,.52, -1,1, 70, true);
    add(Biome_Id::Stone_Desert,  0.85,0.85, 0.20,0.20, -1,1, -1,1, .52,1, -1,1, 45, true);

    // Горные биомы в поиск по T/H не входят: их задаёт сам рельеф.
    add(Biome_Id::Mountains,     .40,.40, .50,.50, .30,.80, .85,.85, .50,.50, -1,1, 65, false);
    add(Biome_Id::High_Mountains,.20,.20, .45,.45, .40,.90, .90,.90, .50,.50, -1,1, 55, false);
    add(Biome_Id::Plateau,       .55,.55, .30,.30, .50,1.0, .75,.75, .50,.50, -1,1, 40, false);
}

// Биом поверхности колонки. Вода/пляж определяются ВЫСОТОЙ относительно уровня
// моря, а не континентальностью: раньше вода наливалась при C < -0.11, а рельеф
// притягивался к морю при C < 0.06, из-за чего между ними оставалась сухая
// плоская полка, а сам "Beach" оказывался под водой.
Biome_Id World_Generator::surface_biome_for_column(const Column& column) const {
    const double h = column.surface_height;

    if (h < kSeaLevel - 16.0) return Biome_Id::Ocean;
    if (h < kSeaLevel - 1.0)  return Biome_Id::Shallow_Water;

    // Полоса пляжа: узкая, у самой воды и только на пологом рельефе, поэтому
    // песок больше не появляется на склоне горы.
    if (h <= kSeaLevel + 3.0 && column.slope < 2.6 && column.temperature > 0.18)
        return Biome_Id::Beach;

    // Горные биомы определяются рельефом, а не климатом. Раньше Mountains требовал
    // erosion = 0.85, но горы в raw_surface_height растут именно там, где эрозия НИЗКАЯ
    // (ruggedness = 1 - smoothstep(erosion)) — поэтому настоящие горы получали ярлык
    // Plains/Forest, а ярлык Mountains доставался случайным точкам.
    {
        const double ruggedness = 1.0 - smoothstep(0.35, 0.85, column.erosion);
        if (ruggedness > 0.40 && h >= kHighMountainHeight) return Biome_Id::High_Mountains;
        if (ruggedness > 0.40 && h >= kMountainHeight)     return Biome_Id::Mountains;
        // Плато — высокая, но ровная местность (высокая эрозия при высокой «горности»).
        if (ruggedness < 0.40 && h >= kPlateauHeight && column.mountainness > 0.55)
            return Biome_Id::Plateau;
    }

    const Biome_Definition* best = nullptr;
    double best_distance = std::numeric_limits<double>::infinity();

    for (const auto& b : m_biomes) {
        if (!b.surface_candidate) continue;

        const double distance =
            sq(interval_distance(column.temperature, b.min_temperature, b.max_temperature)) +
            sq(interval_distance(column.humidity, b.min_humidity, b.max_humidity)) +
            sq(interval_distance(column.continentalness, b.min_continentalness, b.max_continentalness)) +
            sq(interval_distance(column.erosion, b.min_erosion, b.max_erosion)) +
            sq(interval_distance(column.weirdness, b.min_weirdness, b.max_weirdness)) +
            sq(interval_distance(column.depth_parameter, b.min_depth, b.max_depth));

        if (distance < best_distance ||
            (distance == best_distance && best && b.priority > best->priority)) {
            best_distance = distance;
            best = &b;
        }
    }

    if (!best) return Biome_Id::Plains;

    // Slime — редкий прибрежный биом. Раньше высота проверялась по абсолютной шкале
    // «0..20», оставшейся от старого рельефа, — сейчас суша начинается около 63, и
    // условие никогда не выполнялось. Теперь высота считается от уровня моря.
    if (column.height >= kSeaLevel && column.height <= kSeaLevel + 14 &&
        column.continentalness < 0.30 && column.humidity > 0.80 &&
        column.temperature >= 0.55 && column.temperature <= 0.95 &&
        column.erosion >= 0.35 && column.erosion <= 0.75 &&
        column.weirdness > 0.45 && column.near_ocean) {
        return Biome_Id::Slime;
    }

    return best->id;
}

Biome_Id World_Generator::biome_at(const Column& column, int world_y,
                                   double cave_density,
                                   const Cave_Flags& flags) const {
    if (world_y > 200 && column.humidity > 0.70 && column.continentalness < 0.60)
        return Biome_Id::Sky;

    if (world_y > 80 && column.mountainness > 0.75 && column.erosion > 0.75)
        return Biome_Id::Solar;

    if (world_y < -10 && cave_density > kCaveThreshold) {
        if (world_y < -150 && cave_density > 0.75 &&
            !flags.surface_connected && column.temperature > 0.70)
            return Biome_Id::Deep_Underground;
        if (world_y < -15 && world_y >= -150 && cave_density > 0.60 &&
            flags.water_connected && column.humidity > 0.85)
            return Biome_Id::Flooded_Underground;
        return Biome_Id::Underground;
    }

    if (world_y > column.surface_height && world_y <= kSeaLevel) {
        return column.surface_height < kSeaLevel - 16.0
             ? Biome_Id::Ocean : Biome_Id::Shallow_Water;
    }

    return column.biome;
}

// ---------------------------------------------------------------------------
//  Материалы
// ---------------------------------------------------------------------------
Block_Types World_Generator::surface_block_for(int world_x, int world_z,
                                               const Column& column) const {
    // Дешёвый детерминированный бросок вместо std::mt19937 на каждый блок.
    const int chance = deterministic_roll(world_x, world_z, m_seed, 1, 100);
    const double h = column.surface_height;

    // Дно водоёма. Песок у берега, гравий глубже — раньше всё дно было песком
    // независимо от глубины.
    if (h < kSeaLevel - 1.0) {
        if (h >= kSeaLevel - 5.0) return Block_Types::Sand;
        if (h >= kSeaLevel - 14.0) return chance < 70 ? Block_Types::Sand : Block_Types::Gravel;
        if (h >= kSeaLevel - 30.0) return chance < 55 ? Block_Types::Gravel : Block_Types::Stone;
        return chance < 25 ? Block_Types::Gravel : Block_Types::Stone;
    }

    switch (column.biome) {
        case Biome_Id::Beach:
            // Холодный берег — галька/снег, тёплый — песок.
            if (column.temperature < 0.25) return Block_Types::Gravel;
            return chance < 92 ? Block_Types::Sand : Block_Types::Gravel;
        case Biome_Id::Desert:
            return chance < 82 ? Block_Types::Sand : Block_Types::Red_Sand;
        case Biome_Id::Stone_Desert:
        case Biome_Id::Mountains:
        case Biome_Id::High_Mountains:
        case Biome_Id::Solar:
            // Снежная шапка на высоких вершинах.
            if (h > 150.0 && column.temperature < 0.55) return Block_Types::Snow_Block;
            return Block_Types::Stone;
        case Biome_Id::Ice_Fields:
            return chance < 65 ? Block_Types::Snow_Block : Block_Types::Ice;
        case Biome_Id::Tundra:
            return chance < 75 ? Block_Types::Snow_Block : Block_Types::Grass;
        case Biome_Id::Steppe:
        case Biome_Id::Savanna:
            return chance < 35 ? Block_Types::Red_Sand : Block_Types::Grass;
        case Biome_Id::Slime:
            return Block_Types::Slime_Block;
        case Biome_Id::Sky:
            return Block_Types::Cloud_Stone;
        default:
            // Крутой склон не зарастает травой.
            if (column.slope > 6.0) return Block_Types::Stone;
            return Block_Types::Grass;
    }
}

Block_Types World_Generator::subsurface_block_for(int world_x, int world_y, int world_z,
                                                  const Column& column) const {
    const double below_surface = column.surface_height - static_cast<double>(world_y);

    constexpr double kMantleDepth = 4.0;
    if (below_surface <= kMantleDepth) {
        // Под песком лежит песок, а не земля — иначе на обрыве пляжа видна
        // неестественная коричневая прослойка.
        switch (column.biome) {
            case Biome_Id::Beach:
            case Biome_Id::Desert:
                return Block_Types::Sand;
            case Biome_Id::Ocean:
            case Biome_Id::Shallow_Water:
                return column.surface_height >= kSeaLevel - 14.0
                     ? Block_Types::Sand : Block_Types::Gravel;
            case Biome_Id::Mountains:
            case Biome_Id::High_Mountains:
            case Biome_Id::Stone_Desert:
            case Biome_Id::Solar:
                return Block_Types::Stone;
            case Biome_Id::Sky:
                return Block_Types::Cloud_Stone;
            default:
                return Block_Types::Dirt;
        }
    }

    // Шум руды считаем лениво — только ниже мантии (дорогой noise3).
    const double ore = normalize_noise(m_caves.noise3(
        world_x * 0.11, world_y * 0.11, world_z * 0.11));
    if (below_surface >= 250.0) {
        if (ore > 0.84) return Block_Types::Rare_Mineral;
        if (ore > 0.76) return Block_Types::Diamond_Ore;
        return Block_Types::Deep_Stone;
    }
    if (below_surface >= 90.0) {
        if (ore > 0.915) return Block_Types::Diamond_Ore;
        if (ore > 0.86) return Block_Types::Gold_Ore;
        if (ore > 0.74) return Block_Types::Iron_Ore;
        return Block_Types::Stone;
    }
    if (ore > 0.82) return Block_Types::Iron_Ore;
    if (ore > 0.70 && column.biome == Biome_Id::Forest) return Block_Types::Mossy_Stone;
    return Block_Types::Stone;
}

Block_Types World_Generator::block_in_column(const Column& column,
                                             int world_x, int world_y,
                                             int world_z) const {
    const int height = column.height;

    // Над поверхностью. Вода наливается ВЕЗДЕ, где пусто и ниже уровня моря —
    // без проверки континентальности. Именно эта проверка раньше оставляла сухую
    // полосу между линией прибоя и водой и мешала появляться озёрам.
    if (world_y > height) {
        return world_y <= kSeaLevel ? Block_Types::Water : Block_Types::Air;
    }

    // Пещеры. Верхние 4 блока не выгрызаются, поэтому поверхность не дырявая.
    if (world_y < height - 4) {
        // Под дном водоёма оставляем более толстую крышу, чтобы пещера не
        // вскрывала океан и не осушала его.
        const bool underwater = column.surface_height < kSeaLevel + 1.0;
        const int roof = underwater ? 10 : 4;
        if (world_y < height - roof) {
            const double density = cave_density_at(world_x, world_y, world_z,
                                                   column.surface_height);
            if (density > kCaveThreshold) {
                if (world_y < -15 && world_y >= -150 && density > 0.60 &&
                    column.humidity > 0.85 && world_y < kSeaLevel) {
                    const Cave_Flags flags = cave_flags_at(world_x, world_y, world_z);
                    if (flags.water_connected) return Block_Types::Water;
                }
                return Block_Types::Air;
            }
        }
    }

    if (world_y == height) return surface_block_for(world_x, world_z, column);
    return subsurface_block_for(world_x, world_y, world_z, column);
}

// ---------------------------------------------------------------------------
//  Публичный API
// ---------------------------------------------------------------------------
int World_Generator::get_height(int world_x, int world_z) const {
    return column_at(world_x, world_z).height;
}

Block_Types World_Generator::get_block(int world_x, int world_y, int world_z) const {
    if (world_y < Config::world_min_y || world_y > Config::world_max_y)
        return Block_Types::Air;

    if (world_y >= 200 && sky_island_block(world_x, world_y, world_z)) {
        const bool exposed_top = !sky_island_block(world_x, world_y + 1, world_z);
        return exposed_top ? Block_Types::Grass : Block_Types::Cloud_Stone;
    }

    return block_in_column(column_at(world_x, world_z), world_x, world_y, world_z);
}

bool World_Generator::is_solid(int world_x, int world_y, int world_z) const {
    return get_block_props(get_block(world_x, world_y, world_z)).is_solid;
}

World_Sample World_Generator::sample(int world_x, int world_y, int world_z) const {
    const Column column = column_at(world_x, world_z);
    const Cave_Flags flags = (world_y < -10)
        ? cave_flags_at(world_x, world_y, world_z) : Cave_Flags{};

    World_Sample s;
    s.world_x = world_x; s.world_y = world_y; s.world_z = world_z;
    s.temperature = column.temperature;
    s.humidity = column.humidity;
    s.continentalness = column.continentalness;
    s.erosion = column.erosion;
    s.weirdness = column.weirdness;
    s.depth = column.depth_parameter;
    s.surface_height = column.surface_height;
    s.mountainness = column.mountainness;
    s.slope = column.slope;
    s.near_ocean = column.near_ocean;
    s.cave_density = cave_density_at(world_x, world_y, world_z, column.surface_height);
    s.surface_connection = flags.surface_connected ? 1.0 : 0.0;
    s.water_connection = flags.water_connected ? 1.0 : 0.0;
    s.sky_anchor_valid = world_y > 200 && sky_island_block(world_x, world_y, world_z);
    return s;
}

World_Sample World_Generator::sample_with_connections(int world_x, int world_y, int world_z,
                                                      double water_connection,
                                                      double surface_connection) const {
    World_Sample s = sample(world_x, world_y, world_z);
    s.water_connection = clamp01(water_connection);
    s.surface_connection = clamp01(surface_connection);
    return s;
}

std::string World_Generator::get_biome_name(int world_x, int world_y, int world_z) const {
    const Column column = column_at(world_x, world_z);
    const double density = cave_density_at(world_x, world_y, world_z, column.surface_height);
    const Cave_Flags flags = (world_y < -10 && density > kCaveThreshold)
        ? cave_flags_at(world_x, world_y, world_z) : Cave_Flags{};
    return biome_name(biome_at(column, world_y, density, flags));
}

bool World_Generator::cave_at(int world_x, int world_y, int world_z) const {
    const Column column = column_at(world_x, world_z);
    return world_y < column.surface_height - 4.0 &&
           cave_density_at(world_x, world_y, world_z, column.surface_height) > kCaveThreshold;
}

std::mt19937 World_Generator::make_rng(int world_x, int world_z, int salt) const {
    return std::mt19937(static_cast<uint32_t>(
        hash_position(world_x, world_z, m_seed, salt)));
}

// ---------------------------------------------------------------------------
//  Пакетная генерация чанка
// ---------------------------------------------------------------------------
// Раньше Chunk::generate_blocks звал get_block 32768 раз, и каждый вызов заново
// считал ВЕСЬ климат колонки (около 30 выборок шума) плюс поиск связности пещер.
// Здесь колонка считается один раз на (x, z) — 1024 раза вместо 32768 — и дальше
// заполнение столбца это просто сравнения целых чисел.
void World_Generator::generate_chunk(Chunk_Block_Grid& blocks,
                                     int chunk_x, int chunk_y, int chunk_z) const {
    const int start_x = chunk_x * Config::chunk_size;
    const int start_z = chunk_z * Config::chunk_size;
    const int start_y = chunk_y * Config::chunk_height;
    const int end_y = start_y + Config::chunk_height - 1;

    // Генерируем в плоский буфер и один раз строим палитру (assign_flat): запись в палитровую
    // сетку воксель за вокселем (set) на порядки медленнее. thread_local — генерация идёт в
    // воркерах, буфер переиспользуется без аллокаций.
    thread_local std::vector<uint8_t> flat;
    flat.assign(Chunk_Block_Grid::voxel_count, 0);   // 0 = Air

    // Парящие острова живут только выше Y=200 — для всех остальных чанков этот
    // дорогой путь не выполняется вовсе.
    const bool sky_range = end_y >= 200;

    for (int z = 0; z < Config::chunk_size; ++z) {
        for (int x = 0; x < Config::chunk_size; ++x) {
            const int world_x = start_x + x;
            const int world_z = start_z + z;
            const Column column = column_at(world_x, world_z);

            // Весь столбец выше поверхности и выше уровня моря — чистый воздух,
            // заполнять нечего (кроме диапазона парящих островов).
            const int solid_top = std::max(column.height, kSeaLevel);
            if (!sky_range && start_y > solid_top) continue;

            const int y_from = start_y;
            const int y_to = std::min(end_y, sky_range ? end_y : solid_top);

            for (int world_y = y_from; world_y <= y_to; ++world_y) {
                if (world_y < Config::world_min_y || world_y > Config::world_max_y)
                    continue;

                Block_Types block;
                if (sky_range && world_y >= 200 &&
                    sky_island_block(world_x, world_y, world_z)) {
                    block = sky_island_block(world_x, world_y + 1, world_z)
                          ? Block_Types::Cloud_Stone : Block_Types::Grass;
                } else {
                    block = block_in_column(column, world_x, world_y, world_z);
                }

                if (block != Block_Types::Air) {
                    const size_t index = (static_cast<size_t>(world_y - start_y) * Config::chunk_size +
                                          static_cast<size_t>(z)) * Config::chunk_size +
                                         static_cast<size_t>(x);
                    flat[index] = static_cast<uint8_t>(static_cast<Block_ID>(block));
                }
            }
        }
    }

    blocks.assign_flat(flat.data());
}

bool World_Generator::chunk_is_definitely_air(int chunk_x, int chunk_y, int chunk_z) const {
    const int start_y = chunk_y * Config::chunk_height;
    if (start_y <= kSeaLevel) return false;      // может быть вода
    if (start_y + Config::chunk_height - 1 >= 200) return false;  // парящие острова

    // Достаточно проверить углы и центр: рельеф внутри одного чанка не может
    // подскочить выше этой оценки более чем на амплитуду детального шума.
    const int start_x = chunk_x * Config::chunk_size;
    const int start_z = chunk_z * Config::chunk_size;
    const int last = Config::chunk_size - 1;
    int highest = std::numeric_limits<int>::min();
    const int probes[5][2] = {
        {0, 0}, {last, 0}, {0, last}, {last, last},
        {Config::chunk_size / 2, Config::chunk_size / 2}
    };
    for (const auto& p : probes)
        highest = std::max(highest, column_at(start_x + p[0], start_z + p[1]).height);

    // Запас на рельеф между точками замера и на высоту деревьев.
    return start_y > highest + Config::chunk_size + 24;
}

// ---------------------------------------------------------------------------
//  Декорации
// ---------------------------------------------------------------------------
void World_Generator::decorate_column(Chunk& chunk, int world_x, int world_z) const {
    const Column column = column_at(world_x, world_z);
    const int terrain_height = column.height;

    if (terrain_height < Config::world_min_y + 1 ||
        terrain_height > Config::world_max_y - 20)
        return;

    // Ранний выход: если поверхность этой колонки вообще не попадает в вертикальный
    // диапазон чанка (с запасом на высоту дерева), декорировать нечего. Раньше все
    // 17 вертикальных чанков столбца прогоняли полный проход по 48x48 колонкам,
    // хотя поверхность есть только в одном из них.
    const int chunk_min_y = chunk.get_chunk_y() * Config::chunk_height;
    const int chunk_max_y = chunk_min_y + Config::chunk_height - 1;
    constexpr int kMaxStructureHeight = 26;
    if (terrain_height + kMaxStructureHeight < chunk_min_y ||
        terrain_height > chunk_max_y)
        return;

    // Ниже уровня моря ничего не сажаем: растение под водой висело бы в воде.
    if (terrain_height < kSeaLevel) return;

    const Biome_Id biome = column.biome;
    const Block_Types surface = block_in_column(column, world_x, terrain_height, world_z);

    if (surface == Block_Types::Water || surface == Block_Types::Snow_Block ||
        surface == Block_Types::Ice || surface == Block_Types::Slime_Block ||
        surface == Block_Types::Sand || surface == Block_Types::Gravel)
        return;

    auto rng = make_rng(world_x, world_z, 2);
    std::uniform_int_distribution<int> roll(0, 999);

    const int flora_roll = roll(rng);
    int flora_frequency = 0;
    Block_Types flora = Block_Types::Tall_Grass;

    switch (biome) {
        case Biome_Id::Jungle:
            flora_frequency = 42; flora = Block_Types::Fern; break;
        case Biome_Id::Forest:
        case Biome_Id::Birch_Forest:
            flora_frequency = 42; flora = Block_Types::Tall_Grass; break;
        case Biome_Id::Taiga:
        case Biome_Id::Tundra:
            flora_frequency = 65; flora = Block_Types::Fern; break;
        case Biome_Id::Plains:
        case Biome_Id::Steppe:
        case Biome_Id::Savanna:
            flora_frequency = 90;
            flora = (flora_roll & 1) ? Block_Types::Tall_Grass : Block_Types::Flower;
            break;
        case Biome_Id::Slime:
            flora_frequency = 55; flora = Block_Types::Slime_Grass; break;
        default: break;
    }

    // Дикая морковь: маленькие «грядки» (1-5 растений) на траве умеренных биомов — равнины чаще,
    // леса реже. Грядка (Wild_Farmland) и спелая морковь над ней; когда игрок соберёт урожай,
    // грядка исчезает (см. Game::handle_crop_harvest). Принадлежность колонки «пятну» определяется
    // только хэшем координат центра, поэтому решение одинаково при генерации любого соседнего чанка.
    if (surface == Block_Types::Grass &&
        (biome == Biome_Id::Plains || biome == Biome_Id::Forest ||
         biome == Biome_Id::Birch_Forest || biome == Biome_Id::Steppe)) {
        static constexpr int k_dx[5] = {0, 1, -1, 0, 0};
        static constexpr int k_dz[5] = {0, 0, 0, 1, -1};
        bool wild_carrot = false;
        for (int i = 0; i < 5 && !wild_carrot; ++i) {
            const int cx = world_x - k_dx[i];
            const int cz = world_z - k_dz[i];
            const uint32_t h = static_cast<uint32_t>(hash_position(cx, cz, m_seed, 77));
            if (h % 1000u >= static_cast<uint32_t>(Config::wild_carrot_patch_per_mille_plains)) continue;
            const Biome_Id center_biome = (i == 0) ? biome : column_at(cx, cz).biome;
            const bool plains_like = center_biome == Biome_Id::Plains || center_biome == Biome_Id::Steppe;
            const bool forest_like = center_biome == Biome_Id::Forest || center_biome == Biome_Id::Birch_Forest;
            if (!plains_like && !forest_like) continue;
            // Лес: из тех же попаданий оставляем примерно треть.
            if (forest_like && (h / 1000u) % 3u != 0u) continue;
            // Соседние клетки пятна занимаются не всегда — форма неровная.
            if (i != 0 && ((h >> 12) + static_cast<uint32_t>(i) * 7u) % 100u >= 55u) continue;
            wild_carrot = true;
        }
        if (wild_carrot) {
            const auto put = [&](int y, Block_Types type) {
                const int lx = world_x - chunk.get_chunk_x() * Config::chunk_size;
                const int ly = y - chunk_min_y;
                const int lz = world_z - chunk.get_chunk_z() * Config::chunk_size;
                if (lx >= 0 && lx < Config::chunk_size && ly >= 0 && ly < Config::chunk_height &&
                    lz >= 0 && lz < Config::chunk_size) chunk.set_block(lx, ly, lz, type);
            };
            if (block_in_column(column, world_x, terrain_height + 1, world_z) == Block_Types::Air) {
                put(terrain_height, Block_Types::Wild_Farmland);
                put(terrain_height + 1, Block_Types::Carrot_Crop_3);
                flora_frequency = 0; // трава на этой же клетке не нужна
            }
        }
    }

    if (flora_frequency > 0 && flora_roll < flora_frequency) {
        const int y = terrain_height + 1;
        if (y >= Config::world_min_y && y <= Config::world_max_y &&
            block_in_column(column, world_x, y, world_z) == Block_Types::Air) {
            const int lx = world_x - chunk.get_chunk_x() * Config::chunk_size;
            const int ly = y - chunk_min_y;
            const int lz = world_z - chunk.get_chunk_z() * Config::chunk_size;
            if (lx >= 0 && lx < Config::chunk_size &&
                ly >= 0 && ly < Config::chunk_height &&
                lz >= 0 && lz < Config::chunk_size)
                chunk.set_block(lx, ly, lz, flora);
        }
    }

    const int tree_roll = roll(rng);
    int tree_frequency = 0;
    Tree_Type tree_type = Tree_Type::Oak;

    switch (biome) {
        case Biome_Id::Forest:       tree_frequency = 115; tree_type = Tree_Type::Oak; break;
        case Biome_Id::Birch_Forest: tree_frequency = 115; tree_type = Tree_Type::Birch; break;
        case Biome_Id::Taiga:        tree_frequency = 105; tree_type = Tree_Type::Spruce; break;
        case Biome_Id::Jungle:       tree_frequency = 80;  tree_type = Tree_Type::Jungle; break;
        case Biome_Id::Savanna:      tree_frequency = 145; tree_type = Tree_Type::Acacia; break;
        case Biome_Id::Slime:        tree_frequency = 120; tree_type = Tree_Type::Slimewood; break;
        default: break;
    }

    // Дерево не растёт на обрыве — корень должен стоять на пологой земле.
    if (column.slope > 4.0) tree_frequency = 0;

    if (terrain_height >= kSeaLevel && tree_frequency > 0 && tree_roll < tree_frequency) {
        const auto setter = [&](int x, int y, int z, Block_Types type) {
            // Структура ставится только в уже пустой воксель, поэтому дерево
            // никогда не перезаписывает рельеф или столб воды.
            if (get_block(x, y, z) != Block_Types::Air) return;

            const int lx = x - chunk.get_chunk_x() * Config::chunk_size;
            const int ly = y - chunk_min_y;
            const int lz = z - chunk.get_chunk_z() * Config::chunk_size;
            if (lx >= 0 && lx < Config::chunk_size &&
                ly >= 0 && ly < Config::chunk_height &&
                lz >= 0 && lz < Config::chunk_size) {
                chunk.set_block(lx, ly, lz, type);
            }
        };
        Tree_Generator::place_tree(tree_type, world_x, terrain_height, world_z, setter, rng);
    }

    if (biome == Biome_Id::Desert && tree_roll == 0) {
        const auto setter = [&](int x, int y, int z, Block_Types type) {
            const int lx = x - chunk.get_chunk_x() * Config::chunk_size;
            const int ly = y - chunk_min_y;
            const int lz = z - chunk.get_chunk_z() * Config::chunk_size;
            if (lx >= 0 && lx < Config::chunk_size &&
                ly >= 0 && ly < Config::chunk_height &&
                lz >= 0 && lz < Config::chunk_size)
                chunk.set_block(lx, ly, lz, type);
        };
        Tree_Generator::place_tree(Tree_Type::Palm, world_x, terrain_height, world_z, setter, rng);
    }
}
