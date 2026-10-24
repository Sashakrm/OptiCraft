#include "utils/binary/Binary_Json.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>

namespace {
    constexpr char k_magic[4] = {'O', 'C', 'J', 'B'};
    constexpr size_t k_header_size = 20;
    constexpr int k_max_depth = 512;

    enum Tag : uint8_t {
        Tag_Null = 0, Tag_False = 1, Tag_True = 2, Tag_Int = 3,
        Tag_Double = 4, Tag_String = 5, Tag_Array = 6, Tag_Object = 7
    };

    uint32_t fnv1a(const uint8_t* data, size_t size) {
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < size; ++i) {
            h ^= data[i];
            h *= 16777619u;
        }
        return h;
    }

    void put_varint(std::vector<uint8_t>& out, uint64_t v) {
        while (v >= 0x80) {
            out.push_back(static_cast<uint8_t>(v | 0x80));
            v >>= 7;
        }
        out.push_back(static_cast<uint8_t>(v));
    }

    void put_u32(std::vector<uint8_t>& out, uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    uint64_t zigzag(int64_t v) {
        return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
    }

    int64_t unzigzag(uint64_t v) {
        return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
    }

    class Reader {
    public:
        Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

        uint8_t u8() {
            need(1);
            return m_data[m_pos++];
        }

        uint32_t u32() {
            need(4);
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(m_data[m_pos++]) << (8 * i);
            return v;
        }

        uint64_t varint() {
            uint64_t v = 0;
            for (int shift = 0; shift < 64; shift += 7) {
                const uint8_t b = u8();
                v |= static_cast<uint64_t>(b & 0x7F) << shift;
                if (!(b & 0x80)) return v;
            }
            throw std::runtime_error("OCJB: слишком длинный varint");
        }

        const uint8_t* take(size_t n) {
            need(n);
            const uint8_t* p = m_data + m_pos;
            m_pos += n;
            return p;
        }

        size_t pos() const { return m_pos; }
        size_t size() const { return m_size; }

    private:
        void need(size_t n) const {
            if (n > m_size - m_pos) throw std::runtime_error("OCJB: файл обрывается");
        }

        const uint8_t* m_data;
        size_t m_size;
        size_t m_pos = 0;
    };

    class Encoder {
    public:
        // Первый проход: собрать все строки (значения и ключи) в таблицу, один раз каждую.
        void collect(const Json_Value& v) {
            switch (v.type) {
                case Json_Value::Type::String: intern(v.string_value); break;
                case Json_Value::Type::Array:
                    for (const auto& e : v.array_value) collect(e);
                    break;
                case Json_Value::Type::Object:
                    for (const auto& [key, value] : v.object_value) {
                        intern(key);
                        collect(value);
                    }
                    break;
                default: break;
            }
        }

        void write(const Json_Value& v, std::vector<uint8_t>& out) const {
            switch (v.type) {
                case Json_Value::Type::Null: out.push_back(Tag_Null); break;
                case Json_Value::Type::Bool: out.push_back(v.boolean_value ? Tag_True : Tag_False); break;
                case Json_Value::Type::Number: {
                    const double d = v.number_value;
                    if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 9007199254740992.0) {
                        out.push_back(Tag_Int);
                        put_varint(out, zigzag(static_cast<int64_t>(d)));
                    } else {
                        out.push_back(Tag_Double);
                        uint64_t bits;
                        std::memcpy(&bits, &d, sizeof bits);
                        for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
                    }
                    break;
                }
                case Json_Value::Type::String:
                    out.push_back(Tag_String);
                    put_varint(out, index_of(v.string_value));
                    break;
                case Json_Value::Type::Array:
                    out.push_back(Tag_Array);
                    put_varint(out, v.array_value.size());
                    for (const auto& e : v.array_value) write(e, out);
                    break;
                case Json_Value::Type::Object:
                    out.push_back(Tag_Object);
                    put_varint(out, v.object_value.size());
                    for (const auto& [key, value] : v.object_value) {
                        put_varint(out, index_of(key));
                        write(value, out);
                    }
                    break;
            }
        }

        const std::vector<std::string>& strings() const { return m_strings; }

    private:
        void intern(const std::string& s) {
            if (m_index.emplace(s, static_cast<uint32_t>(m_strings.size())).second) {
                m_strings.push_back(s);
            }
        }

        uint64_t index_of(const std::string& s) const { return m_index.at(s); }

        std::vector<std::string> m_strings;
        std::unordered_map<std::string, uint32_t> m_index;
    };

    Json_Value read_value(Reader& r, const std::vector<std::string>& strings, int depth) {
        if (depth > k_max_depth) throw std::runtime_error("OCJB: слишком глубокое дерево");
        Json_Value v;
        const uint8_t tag = r.u8();
        switch (tag) {
            case Tag_Null: v.type = Json_Value::Type::Null; break;
            case Tag_False: v.type = Json_Value::Type::Bool; v.boolean_value = false; break;
            case Tag_True: v.type = Json_Value::Type::Bool; v.boolean_value = true; break;
            case Tag_Int:
                v.type = Json_Value::Type::Number;
                v.number_value = static_cast<double>(unzigzag(r.varint()));
                break;
            case Tag_Double: {
                const uint8_t* p = r.take(8);
                uint64_t bits = 0;
                for (int i = 0; i < 8; ++i) bits |= static_cast<uint64_t>(p[i]) << (8 * i);
                double d;
                std::memcpy(&d, &bits, sizeof d);
                v.type = Json_Value::Type::Number;
                v.number_value = d;
                break;
            }
            case Tag_String: {
                const uint64_t idx = r.varint();
                if (idx >= strings.size()) throw std::runtime_error("OCJB: индекс строки вне таблицы");
                v.type = Json_Value::Type::String;
                v.string_value = strings[idx];
                break;
            }
            case Tag_Array: {
                const uint64_t n = r.varint();
                if (n > r.size()) throw std::runtime_error("OCJB: неверное число элементов массива");
                v.type = Json_Value::Type::Array;
                v.array_value.reserve(n);
                for (uint64_t i = 0; i < n; ++i) v.array_value.push_back(read_value(r, strings, depth + 1));
                break;
            }
            case Tag_Object: {
                const uint64_t n = r.varint();
                if (n > r.size()) throw std::runtime_error("OCJB: неверное число полей объекта");
                v.type = Json_Value::Type::Object;
                v.object_value.reserve(n);
                for (uint64_t i = 0; i < n; ++i) {
                    const uint64_t kidx = r.varint();
                    if (kidx >= strings.size()) throw std::runtime_error("OCJB: индекс ключа вне таблицы");
                    v.object_value.emplace(strings[kidx], read_value(r, strings, depth + 1));
                }
                break;
            }
            default: throw std::runtime_error("OCJB: неизвестный тег значения");
        }
        return v;
    }
} // namespace

namespace Binary_Json {

std::vector<uint8_t> encode(const Json_Value& root) {
    Encoder enc;
    enc.collect(root);

    // Полезная нагрузка: таблица строк, затем дерево.
    std::vector<uint8_t> payload;
    for (const std::string& s : enc.strings()) {
        put_varint(payload, s.size());
        payload.insert(payload.end(), s.begin(), s.end());
    }
    enc.write(root, payload);

    std::vector<uint8_t> out;
    out.reserve(k_header_size + payload.size());
    out.insert(out.end(), std::begin(k_magic), std::end(k_magic));
    out.push_back(static_cast<uint8_t>(k_version & 0xFF));
    out.push_back(static_cast<uint8_t>(k_version >> 8));
    out.push_back(0);
    out.push_back(0);
    put_u32(out, fnv1a(payload.data(), payload.size()));
    put_u32(out, static_cast<uint32_t>(enc.strings().size()));
    put_u32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Json_Value decode(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < k_header_size) throw std::runtime_error("OCJB: файл слишком короткий");
    if (std::memcmp(bytes.data(), k_magic, 4) != 0) throw std::runtime_error("OCJB: неверная сигнатура");
    const uint16_t version = static_cast<uint16_t>(bytes[4] | (bytes[5] << 8));
    if (version != k_version) throw std::runtime_error("OCJB: неподдерживаемая версия");

    Reader header(bytes.data() + 8, 12);
    const uint32_t checksum = header.u32();
    const uint32_t string_count = header.u32();
    const uint32_t payload_size = header.u32();
    if (payload_size != bytes.size() - k_header_size) throw std::runtime_error("OCJB: размер не совпадает");

    const uint8_t* payload = bytes.data() + k_header_size;
    if (fnv1a(payload, payload_size) != checksum) throw std::runtime_error("OCJB: контрольная сумма не совпадает");

    Reader r(payload, payload_size);
    std::vector<std::string> strings;
    strings.reserve(string_count);
    for (uint32_t i = 0; i < string_count; ++i) {
        const uint64_t len = r.varint();
        const uint8_t* p = r.take(static_cast<size_t>(len));
        strings.emplace_back(reinterpret_cast<const char*>(p), static_cast<size_t>(len));
    }
    Json_Value root = read_value(r, strings, 0);
    if (r.pos() != payload_size) throw std::runtime_error("OCJB: лишние байты в конце");
    return root;
}

void save_file(const Json_Value& root, const std::string& path) {
    const std::vector<uint8_t> bytes = encode(root);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("OCJB: не удалось открыть для записи: " + path);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("OCJB: ошибка записи: " + path);
}

Json_Value load_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("OCJB: не удалось открыть: " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decode(bytes);
}

} // namespace Binary_Json
