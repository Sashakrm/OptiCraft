#ifndef OPTICRAFT_REGION_FILE_H
#define OPTICRAFT_REGION_FILE_H

#include "world/chunks/Chunk.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Все чанки одного пресета генерации лежат в ОДНОМ файле вместо отдельного
// c_x_y_z.bin на каждый чанк.
//
// Зачем: при радиусе загрузки 5 и 17 вертикальных чанках вокруг игрока живёт
// больше двух тысяч чанков. Отдельными файлами это две тысячи inode, две тысячи
// open/write/close/rename на каждый проход выгрузки, и каталог, который файловая
// система перестаёт нормально кэшировать. Одним файлом это одна ручка, один seek
// и одна запись.
//
// Формат (little-endian, как и раньше — движок собирается только под LE):
//
//   [Заголовок, 64 байта]
//     "OPCR"            4   магия
//     version           4   = 1
//     sector_size       4   = 512
//     index_offset      8   смещение таблицы индекса в байтах
//     index_count       4   число записей в индексе
//     size_x/y/z        4*3 размеры чанка, под которые писался файл
//     reserved          28
//
//   [Сектора с данными чанков]
//
//   [Таблица индекса: index_count записей по 32 байта]
//     int32 x, y, z         координаты чанка
//     uint32 first_sector   смещение данных в секторах
//     uint32 sector_count   сколько секторов занято
//     uint32 payload_bytes  длина полезных данных
//     uint32 format         0 = сырые байты, 1 = RLE
//     uint32 reserved
//
// Устойчивость к падению: индекс пишется в НОВОЕ место в конце файла, и только
// после успешного сброса данных на диск обновляется index_offset в заголовке.
// Если процесс умрёт посередине, заголовок всё ещё указывает на прошлый
// валидный индекс, и мир откатится ровно на одно сохранение, а не развалится.
class Region_File {
public:
    struct Key {
        int x = 0, y = 0, z = 0;
        bool operator==(const Key& other) const {
            return x == other.x && y == other.y && z == other.z;
        }
    };

    struct Key_Hash {
        size_t operator()(const Key& k) const noexcept {
            uint64_t h = static_cast<uint64_t>(static_cast<int64_t>(k.x)) * 0x9E3779B185EBCA87ull;
            h ^= static_cast<uint64_t>(static_cast<int64_t>(k.y)) * 0xC2B2AE3D27D4EB4Full;
            h ^= static_cast<uint64_t>(static_cast<int64_t>(k.z)) * 0x165667B19E3779F9ull;
            h ^= h >> 29;
            return static_cast<size_t>(h ^ (h >> 32));
        }
    };

    // Один Region_File на путь. World_File копируется по значению внутрь
    // Chunk_Manager, поэтому сам файл живёт отдельно и разделяется через
    // shared_ptr — иначе две копии World_File открыли бы один файл дважды.
    static std::shared_ptr<Region_File> get(const std::filesystem::path& path);
    static void flush_all();
    // Забыть открытые region-файлы внутри каталога (перед удалением мира с диска).
    static void forget_under(const std::filesystem::path& directory);

    explicit Region_File(std::filesystem::path path);
    ~Region_File();

    Region_File(const Region_File&) = delete;
    Region_File& operator=(const Region_File&) = delete;

    bool read(const Key& key, Chunk_Block_Grid& blocks);
    bool write(const Key& key, const Chunk_Block_Grid& blocks);
    bool contains(const Key& key);
    // Сбрасывает индекс и данные на диск. Дёргается на выгрузке мира и при
    // выходе; между этими моментами индекс живёт в памяти.
    bool flush();

    size_t get_chunk_count();

private:
    struct Entry {
        uint32_t first_sector = 0;
        uint32_t sector_count = 0;
        uint32_t payload_bytes = 0;
        uint32_t format = 0;
    };

    struct Free_Range {
        uint32_t first_sector = 0;
        uint32_t sector_count = 0;
    };

    static constexpr uint32_t k_sector_size = 512;
    static constexpr uint32_t k_header_size = 64;
    static constexpr uint32_t k_index_entry_size = 32;
    static constexpr uint32_t k_version = 1;
    static constexpr uint32_t k_format_raw = 0;
    static constexpr uint32_t k_format_rle = 1;
    // 2 = блоки (RLE или сырые) + разреженные метаданные блоков (ориентация/активность).
    // Пишется ТОЛЬКО для чанков, где метаданные есть; остальные остаются форматами 0/1,
    // так что старые миры читаются как раньше, а мир без метаданных не растёт.
    //   [uint8 кодировка блоков: 0 сырые / 1 RLE] [uint32 длина блоков] [блоки]
    //   [uint32 число записей] [записи: uint16 индекс вокселя, uint8 значение]...
    static constexpr uint32_t k_format_blocks_meta = 2;

    std::filesystem::path m_path;
    std::fstream m_stream;
    std::unordered_map<Key, Entry, Key_Hash> m_index;
    std::vector<Free_Range> m_free_list;
    uint32_t m_next_sector = 0;       // первый сектор за концом занятых данных
    bool m_index_dirty = false;
    bool m_open = false;
    std::mutex m_mutex;

    // Буферы переиспользуются между вызовами, чтобы сохранение чанка не
    // аллоцировало по 32 КБ каждый раз.
    std::vector<uint8_t> m_scratch_compress;
    std::vector<uint8_t> m_scratch_read;

    bool open_locked();
    bool create_empty_locked();
    bool read_header_locked();
    bool write_header_locked(uint64_t index_offset, uint32_t index_count);
    bool read_index_locked(uint64_t index_offset, uint32_t index_count);

    uint32_t allocate_sectors_locked(uint32_t sector_count);
    void release_sectors_locked(uint32_t first_sector, uint32_t sector_count);

    // RLE: (uint8 значение, uint16 длина серии). Данные чанка почти всегда
    // состоят из длинных однородных полос — чанк чистого воздуха ужимается
    // с 32768 байт до трёх. Если RLE вдруг вышел длиннее сырых данных
    // (шумный чанк с рудами), пишутся сырые байты.
    static void compress_rle(const uint8_t* source, size_t source_size,
                             std::vector<uint8_t>& out);
    static bool decompress_rle(const uint8_t* source, size_t source_size,
                               uint8_t* destination, size_t destination_size);
};

#endif // OPTICRAFT_REGION_FILE_H
