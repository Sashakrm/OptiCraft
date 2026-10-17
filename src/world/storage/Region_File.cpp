#include "world/storage/Region_File.h"

#include "utils/logging/Logger.h"

#include <algorithm>
#include <cstring>

namespace {
    std::mutex g_registry_mutex;
    // Сильные ссылки, а не weak: World_File копируется по значению и может на
    // мгновение остаться единственным владельцем. С weak_ptr объект в этот момент
    // умер бы, и следующий вызов заново открывал бы файл и перечитывал индекс.
    std::unordered_map<std::string, std::shared_ptr<Region_File>> g_registry;

    void write_u32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }
    void write_i32(uint8_t* p, int32_t v) { std::memcpy(p, &v, sizeof(v)); }
    void write_u64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, sizeof(v)); }
    uint32_t read_u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, sizeof(v)); return v; }
    int32_t read_i32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, sizeof(v)); return v; }
    uint64_t read_u64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, sizeof(v)); return v; }
}

std::shared_ptr<Region_File> Region_File::get(const std::filesystem::path& path) {
    const std::string key = path.string();
    std::lock_guard<std::mutex> lock(g_registry_mutex);

    auto it = g_registry.find(key);
    if (it != g_registry.end()) return it->second;

    auto created = std::make_shared<Region_File>(path);
    g_registry[key] = created;
    return created;
}

void Region_File::forget_under(const std::filesystem::path& directory) {
    const std::string prefix = directory.string();
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (auto it = g_registry.begin(); it != g_registry.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) it = g_registry.erase(it);
        else ++it;
    }
}

void Region_File::flush_all() {
    std::vector<std::shared_ptr<Region_File>> alive;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        alive.reserve(g_registry.size());
        for (auto& [key, file] : g_registry) alive.push_back(file);
    }
    for (auto& file : alive) file->flush();
}

Region_File::Region_File(std::filesystem::path path)
    : m_path(std::move(path))
{}

Region_File::~Region_File() {
    flush();
}

// ---------------------------------------------------------------------------
//  Сжатие
// ---------------------------------------------------------------------------
namespace {
    // RLE: (значение шириной sizeof(T), uint16 длина серии). Данные чанка почти всегда состоят
    // из длинных однородных полос — чанк чистого воздуха ужимается до нескольких байт.
    template <typename T>
    void compress_rle(const T* source, size_t count, std::vector<uint8_t>& out) {
        out.clear();
        out.reserve(count * sizeof(T) / 8 + 16);
        size_t i = 0;
        while (i < count) {
            const T value = source[i];
            size_t run = 1;
            // Длина серии ограничена 65535, чтобы влезть в uint16.
            while (i + run < count && source[i + run] == value && run < 0xFFFFu) {
                ++run;
            }
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(T));
            out.push_back(static_cast<uint8_t>(run & 0xFFu));
            out.push_back(static_cast<uint8_t>((run >> 8) & 0xFFu));
            i += run;
        }
    }

    template <typename T>
    bool decompress_rle(const uint8_t* source, size_t source_size, T* destination, size_t destination_count) {
        constexpr size_t k_entry = sizeof(T) + 2;
        size_t written = 0;
        size_t i = 0;
        while (i + k_entry <= source_size) {
            T value{};
            std::memcpy(&value, source + i, sizeof(T));
            const size_t run = static_cast<size_t>(source[i + sizeof(T)]) |
                               (static_cast<size_t>(source[i + sizeof(T) + 1]) << 8);
            i += k_entry;
            if (run == 0 || written + run > destination_count) return false;
            std::fill_n(destination + written, run, value);
            written += run;
        }
        return i == source_size && written == destination_count;
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Открытие / заголовок / индекс
// ---------------------------------------------------------------------------
bool Region_File::create_empty_locked() {
    std::error_code error;
    const auto directory = m_path.parent_path();
    if (!directory.empty()) {
        std::filesystem::create_directories(directory, error);
        if (error) {
            LOG_ERROR("Region_File: cannot create directory " + directory.string() +
                      ": " + error.message());
            return false;
        }
    }

    {
        std::ofstream create(m_path, std::ios::binary | std::ios::trunc);
        if (!create) {
            LOG_ERROR("Region_File: cannot create " + m_path.string());
            return false;
        }
    }

    m_stream.open(m_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!m_stream) {
        LOG_ERROR("Region_File: cannot open " + m_path.string());
        return false;
    }

    m_index.clear();
    m_free_list.clear();
    // Сектор 0 занят заголовком.
    m_next_sector = 1;

    // Индекс пуст, но заголовок должен быть валидным сразу.
    if (!write_header_locked(static_cast<uint64_t>(m_next_sector) * k_sector_size, 0)) {
        return false;
    }
    m_stream.flush();
    m_open = true;
    return true;
}

bool Region_File::read_header_locked() {
    uint8_t header[k_header_size]{};
    m_stream.seekg(0, std::ios::beg);
    m_stream.read(reinterpret_cast<char*>(header), k_header_size);
    if (!m_stream) return false;

    if (std::memcmp(header, "OPCR", 4) != 0) return false;
    if (read_u32(header + 4) != k_version) return false;
    if (read_u32(header + 8) != k_sector_size) return false;

    const uint64_t index_offset = read_u64(header + 12);
    const uint32_t index_count = read_u32(header + 20);

    const uint32_t size_x = read_u32(header + 24);
    const uint32_t size_y = read_u32(header + 28);
    const uint32_t size_z = read_u32(header + 32);
    if (size_x != Config::chunk_size || size_y != Config::chunk_height ||
        size_z != Config::chunk_size) {
        LOG_WARN("Region_File: chunk dimensions changed, ignoring " + m_path.string());
        return false;
    }

    return read_index_locked(index_offset, index_count);
}

bool Region_File::write_header_locked(uint64_t index_offset, uint32_t index_count) {
    uint8_t header[k_header_size]{};
    std::memcpy(header, "OPCR", 4);
    write_u32(header + 4, k_version);
    write_u32(header + 8, k_sector_size);
    write_u64(header + 12, index_offset);
    write_u32(header + 20, index_count);
    write_u32(header + 24, static_cast<uint32_t>(Config::chunk_size));
    write_u32(header + 28, static_cast<uint32_t>(Config::chunk_height));
    write_u32(header + 32, static_cast<uint32_t>(Config::chunk_size));

    m_stream.seekp(0, std::ios::beg);
    m_stream.write(reinterpret_cast<const char*>(header), k_header_size);
    return static_cast<bool>(m_stream);
}

bool Region_File::read_index_locked(uint64_t index_offset, uint32_t index_count) {
    m_index.clear();
    m_free_list.clear();
    m_next_sector = 1;

    if (index_count == 0) {
        m_next_sector = static_cast<uint32_t>(index_offset / k_sector_size);
        if (m_next_sector < 1) m_next_sector = 1;
        return true;
    }

    std::vector<uint8_t> table(static_cast<size_t>(index_count) * k_index_entry_size);
    m_stream.seekg(static_cast<std::streamoff>(index_offset), std::ios::beg);
    m_stream.read(reinterpret_cast<char*>(table.data()),
                  static_cast<std::streamsize>(table.size()));
    if (!m_stream) {
        LOG_WARN("Region_File: truncated index in " + m_path.string());
        m_stream.clear();
        return false;
    }

    uint32_t highest_sector = 1;
    for (uint32_t i = 0; i < index_count; ++i) {
        const uint8_t* p = table.data() + static_cast<size_t>(i) * k_index_entry_size;
        Key key;
        key.x = read_i32(p + 0);
        key.y = read_i32(p + 4);
        key.z = read_i32(p + 8);

        Entry entry;
        entry.first_sector = read_u32(p + 12);
        entry.sector_count = read_u32(p + 16);
        entry.payload_bytes = read_u32(p + 20);
        entry.format = read_u32(p + 24);

        if (entry.sector_count == 0) continue;
        m_index[key] = entry;
        highest_sector = std::max(highest_sector, entry.first_sector + entry.sector_count);
    }

    // Старая таблица индекса становится свободным местом: её сектора можно
    // переиспользовать под данные при следующих записях.
    m_next_sector = highest_sector;
    const uint32_t index_first_sector = static_cast<uint32_t>(index_offset / k_sector_size);
    if (index_first_sector >= m_next_sector) {
        const uint64_t index_bytes = static_cast<uint64_t>(index_count) * k_index_entry_size;
        const uint32_t index_sectors =
            static_cast<uint32_t>((index_bytes + k_sector_size - 1) / k_sector_size);
        if (index_first_sector > m_next_sector) {
            release_sectors_locked(m_next_sector, index_first_sector - m_next_sector);
        }
        m_next_sector = index_first_sector + index_sectors;
    }
    return true;
}

bool Region_File::open_locked() {
    if (m_open) return true;

    std::error_code error;
    const bool exists = std::filesystem::exists(m_path, error) && !error;

    if (exists) {
        m_stream.open(m_path, std::ios::binary | std::ios::in | std::ios::out);
        if (m_stream && read_header_locked()) {
            m_open = true;
            return true;
        }
        // Файл есть, но нечитаем или от другой версии — не затираем его молча.
        if (m_stream) m_stream.close();
        m_stream.clear();

        const auto backup = m_path.string() + ".broken";
        std::filesystem::rename(m_path, backup, error);
        LOG_WARN("Region_File: unreadable region moved to " + backup);
    }

    return create_empty_locked();
}

// ---------------------------------------------------------------------------
//  Аллокатор секторов
// ---------------------------------------------------------------------------
uint32_t Region_File::allocate_sectors_locked(uint32_t sector_count) {
    // Первое подходящее свободное место, иначе — конец файла.
    for (size_t i = 0; i < m_free_list.size(); ++i) {
        if (m_free_list[i].sector_count < sector_count) continue;

        const uint32_t first = m_free_list[i].first_sector;
        if (m_free_list[i].sector_count == sector_count) {
            m_free_list.erase(m_free_list.begin() + static_cast<long>(i));
        } else {
            m_free_list[i].first_sector += sector_count;
            m_free_list[i].sector_count -= sector_count;
        }
        return first;
    }

    const uint32_t first = m_next_sector;
    m_next_sector += sector_count;
    return first;
}

void Region_File::release_sectors_locked(uint32_t first_sector, uint32_t sector_count) {
    if (sector_count == 0) return;

    m_free_list.push_back({first_sector, sector_count});
    std::sort(m_free_list.begin(), m_free_list.end(),
              [](const Free_Range& a, const Free_Range& b) {
                  return a.first_sector < b.first_sector;
              });

    // Склеиваем соседние диапазоны, иначе файл со временем фрагментируется
    // в тысячу кусков по одному сектору.
    std::vector<Free_Range> merged;
    merged.reserve(m_free_list.size());
    for (const auto& range : m_free_list) {
        if (!merged.empty() &&
            merged.back().first_sector + merged.back().sector_count == range.first_sector) {
            merged.back().sector_count += range.sector_count;
        } else {
            merged.push_back(range);
        }
    }
    m_free_list.swap(merged);
}

// ---------------------------------------------------------------------------
//  Чтение / запись
// ---------------------------------------------------------------------------
bool Region_File::contains(const Key& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!open_locked()) return false;
    return m_index.find(key) != m_index.end();
}

bool Region_File::read(const Key& key, Chunk_Block_Grid& blocks) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!open_locked()) return false;

    auto it = m_index.find(key);
    if (it == m_index.end()) return false;

    const Entry entry = it->second;
    if (entry.payload_bytes == 0) return false;

    m_scratch_read.resize(entry.payload_bytes);
    m_stream.seekg(static_cast<std::streamoff>(entry.first_sector) * k_sector_size,
                   std::ios::beg);
    m_stream.read(reinterpret_cast<char*>(m_scratch_read.data()),
                  static_cast<std::streamsize>(entry.payload_bytes));
    if (!m_stream) {
        m_stream.clear();
        LOG_WARN("Region_File: short read for chunk " + std::to_string(key.x) + "," +
                 std::to_string(key.y) + "," + std::to_string(key.z));
        return false;
    }

    blocks.clear_meta();

    // Внутри чанк хранится палитрой (см. Chunk_Block_Grid). На диске — плоские id: новые форматы
    // пишут uint16 на воксель, старые (0..2) — байт на воксель; оба читаются и приводятся к Block_ID.
    std::vector<Block_ID>& flat = m_scratch_flat;
    flat.resize(Chunk_Block_Grid::voxel_count);
    const uint8_t* p = m_scratch_read.data();
    const size_t total = m_scratch_read.size();

    // Декодирует блоки из буфера нужной ширины: wide — по uint16 на воксель, иначе по байту.
    const auto decode = [&flat](const uint8_t* data, size_t length, bool rle, bool wide) -> bool {
        const size_t count = flat.size();
        if (wide) {
            if (rle) return decompress_rle<Block_ID>(data, length, flat.data(), count);
            if (length != count * sizeof(Block_ID)) return false;
            std::memcpy(flat.data(), data, length);
            return true;
        }
        std::vector<uint8_t> narrow(count);
        if (rle) {
            if (!decompress_rle<uint8_t>(data, length, narrow.data(), count)) return false;
        } else {
            if (length != count) return false;
            std::memcpy(narrow.data(), data, count);
        }
        for (size_t k = 0; k < count; ++k) flat[k] = narrow[k];
        return true;
    };
    // Id, которых нет в blocks.json (битые или устаревшие данные), превращаем в воздух.
    const auto sanitize = [&flat]() {
        const Block_Registry& registry = Block_Registry::get_instance();
        for (Block_ID& id : flat) {
            if (!registry.is_known(id)) id = static_cast<Block_ID>(Block_Types::Air);
        }
    };

    if (entry.format == k_format_raw || entry.format == k_format_rle) {
        if (entry.format == k_format_raw && entry.payload_bytes != Chunk_Block_Grid::voxel_count) return false;
        if (!decode(p, total, entry.format == k_format_rle, false)) return false;
        sanitize();
        blocks.assign_flat(flat.data());
        return true;
    }
    if (entry.format == k_format_raw16 || entry.format == k_format_rle16) {
        if (!decode(p, total, entry.format == k_format_rle16, true)) return false;
        sanitize();
        blocks.assign_flat(flat.data());
        return true;
    }
    if (entry.format == k_format_blocks_meta || entry.format == k_format_blocks_meta16) {
        if (total < 1 + 4) return false;
        const uint8_t encoding = p[0];
        if (encoding > 1) return false;
        uint32_t blocks_len = 0;
        std::memcpy(&blocks_len, p + 1, sizeof(blocks_len));
        size_t offset = 1 + 4;
        if (offset + blocks_len + 4 > total) return false;
        if (!decode(p + offset, blocks_len, encoding == 1, entry.format == k_format_blocks_meta16)) return false;
        sanitize();
        blocks.assign_flat(flat.data());   // сбрасывает метаданные — их читаем ПОСЛЕ
        offset += blocks_len;

        uint32_t meta_count = 0;
        std::memcpy(&meta_count, p + offset, sizeof(meta_count));
        offset += 4;
        if (offset + static_cast<size_t>(meta_count) * 3 > total) return false;
        for (uint32_t k = 0; k < meta_count; ++k) {
            uint16_t index = 0;
            std::memcpy(&index, p + offset, sizeof(index));
            blocks.set_meta_by_index(index, p[offset + 2]);
            offset += 3;
        }
        return true;
    }
    return false;
}

bool Region_File::write(const Key& key, const Chunk_Block_Grid& blocks) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!open_locked()) return false;

    // Пишем всегда 16-битными id: ограничения на число видов блоков на диске нет.
    std::vector<Block_ID>& flat = m_scratch_flat;
    flat.resize(Chunk_Block_Grid::voxel_count);
    blocks.unpack(flat.data());
    const size_t raw_bytes = flat.size() * sizeof(Block_ID);
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(flat.data());

    compress_rle<Block_ID>(flat.data(), flat.size(), m_scratch_compress);
    // Сжатый вариант берём, только если он короче сырого (шумный чанк с рудами — сырой).
    const bool use_rle = m_scratch_compress.size() < raw_bytes;
    const uint8_t* block_bytes = use_rle ? m_scratch_compress.data() : raw;
    const uint32_t block_len = static_cast<uint32_t>(use_rle ? m_scratch_compress.size() : raw_bytes);

    const uint8_t* payload = block_bytes;
    uint32_t payload_bytes = block_len;
    uint32_t format = use_rle ? k_format_rle16 : k_format_raw16;

    // Чанк с метаданными блоков: оборачиваем блоки (как выбраны выше) и дописываем метаданные.
    std::vector<uint8_t> meta_payload;
    if (blocks.has_meta()) {
        const auto& entries = blocks.meta_entries();
        const uint32_t meta_count = static_cast<uint32_t>(entries.size());
        meta_payload.reserve(1 + 4 + block_len + 4 + static_cast<size_t>(meta_count) * 3);
        meta_payload.push_back(use_rle ? 1 : 0);
        const auto append_u32 = [&meta_payload](uint32_t v) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
            meta_payload.insert(meta_payload.end(), b, b + sizeof(v));
        };
        append_u32(block_len);
        meta_payload.insert(meta_payload.end(), block_bytes, block_bytes + block_len);
        append_u32(meta_count);
        for (const auto& [index, value] : entries) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&index);
            meta_payload.insert(meta_payload.end(), b, b + sizeof(index));
            meta_payload.push_back(value);
        }
        payload = meta_payload.data();
        payload_bytes = static_cast<uint32_t>(meta_payload.size());
        format = k_format_blocks_meta16;
    }

    const uint32_t needed_sectors = (payload_bytes + k_sector_size - 1) / k_sector_size;

    Entry entry;
    auto it = m_index.find(key);
    if (it != m_index.end() && it->second.sector_count >= needed_sectors) {
        // Влезает в уже выделенное место — переписываем на месте и отдаём
        // обратно лишний хвост.
        entry.first_sector = it->second.first_sector;
        if (it->second.sector_count > needed_sectors) {
            release_sectors_locked(entry.first_sector + needed_sectors,
                                   it->second.sector_count - needed_sectors);
        }
    } else {
        if (it != m_index.end()) {
            release_sectors_locked(it->second.first_sector, it->second.sector_count);
        }
        entry.first_sector = allocate_sectors_locked(needed_sectors);
    }

    entry.sector_count = needed_sectors;
    entry.payload_bytes = payload_bytes;
    entry.format = format;

    m_stream.seekp(static_cast<std::streamoff>(entry.first_sector) * k_sector_size,
                   std::ios::beg);
    m_stream.write(reinterpret_cast<const char*>(payload), payload_bytes);
    if (!m_stream) {
        m_stream.clear();
        LOG_ERROR("Region_File: write failed for chunk " + std::to_string(key.x) + "," +
                  std::to_string(key.y) + "," + std::to_string(key.z));
        return false;
    }

    m_index[key] = entry;
    m_index_dirty = true;
    return true;
}

bool Region_File::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_open || !m_index_dirty) return true;

    // Индекс пишется в новое место в конце файла. Заголовок обновляется
    // последним, поэтому падение между двумя записями оставляет валидным
    // предыдущий индекс.
    const uint32_t index_count = static_cast<uint32_t>(m_index.size());
    std::vector<uint8_t> table(static_cast<size_t>(index_count) * k_index_entry_size, 0);

    size_t i = 0;
    for (const auto& [key, entry] : m_index) {
        uint8_t* p = table.data() + i * k_index_entry_size;
        write_i32(p + 0, key.x);
        write_i32(p + 4, key.y);
        write_i32(p + 8, key.z);
        write_u32(p + 12, entry.first_sector);
        write_u32(p + 16, entry.sector_count);
        write_u32(p + 20, entry.payload_bytes);
        write_u32(p + 24, entry.format);
        ++i;
    }

    const uint32_t index_sectors =
        static_cast<uint32_t>((table.size() + k_sector_size - 1) / k_sector_size);
    const uint32_t index_first_sector = m_next_sector;
    const uint64_t index_offset = static_cast<uint64_t>(index_first_sector) * k_sector_size;

    if (!table.empty()) {
        m_stream.seekp(static_cast<std::streamoff>(index_offset), std::ios::beg);
        m_stream.write(reinterpret_cast<const char*>(table.data()),
                       static_cast<std::streamsize>(table.size()));
        if (!m_stream) {
            m_stream.clear();
            LOG_ERROR("Region_File: cannot write index for " + m_path.string());
            return false;
        }
    }
    m_stream.flush();

    if (!write_header_locked(index_offset, index_count)) {
        LOG_ERROR("Region_File: cannot write header for " + m_path.string());
        return false;
    }
    m_stream.flush();

    // Место под индекс теперь занято; следующие данные пойдут за ним.
    m_next_sector = index_first_sector + index_sectors;
    m_index_dirty = false;
    return true;
}

size_t Region_File::get_chunk_count() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!open_locked()) return 0;
    return m_index.size();
}
