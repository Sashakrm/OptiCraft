// json2bin — инструмент для формата OCJB (см. src/utils/binary/Binary_Json.h).
//
//   json2bin <in.json> [out.ocb]    перевести один файл (по умолчанию рядом, расширение .ocb)
//   json2bin --dir <каталог>        перевести все *.json в каталоге рекурсивно
//   json2bin --verify <in.json>     перевести в память, декодировать обратно, сравнить дерево
//                                   и показать размер и время: текстовый разбор против бинарного
//   json2bin --dump <in.ocb>        прочитать бинарный файл и напечатать JSON (чтение бинарников)

#include "utils/binary/Binary_Json.h"
#include "utils/text/Json.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {
    std::string read_text(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("не удалось открыть: " + path);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::uintmax_t file_size(const std::string& path) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        return ec ? 0 : size;
    }

    bool deep_equal(const Json_Value& a, const Json_Value& b) {
        if (a.type != b.type) return false;
        switch (a.type) {
            case Json_Value::Type::Null: return true;
            case Json_Value::Type::Bool: return a.boolean_value == b.boolean_value;
            case Json_Value::Type::Number:
                return a.number_value == b.number_value ||
                       (std::isnan(a.number_value) && std::isnan(b.number_value));
            case Json_Value::Type::String: return a.string_value == b.string_value;
            case Json_Value::Type::Array:
                if (a.array_value.size() != b.array_value.size()) return false;
                for (size_t i = 0; i < a.array_value.size(); ++i)
                    if (!deep_equal(a.array_value[i], b.array_value[i])) return false;
                return true;
            case Json_Value::Type::Object:
                if (a.object_value.size() != b.object_value.size()) return false;
                for (const auto& [key, value] : a.object_value) {
                    const Json_Value* other = b.find(key);
                    if (!other || !deep_equal(value, *other)) return false;
                }
                return true;
        }
        return false;
    }

    void escape_into(const std::string& s, std::string& out) {
        out.push_back('"');
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
        }
        out.push_back('"');
    }

    void dump(const Json_Value& v, std::string& out, int indent) {
        const std::string pad(static_cast<size_t>(indent) * 2, ' ');
        const std::string inner(static_cast<size_t>(indent + 1) * 2, ' ');
        switch (v.type) {
            case Json_Value::Type::Null: out += "null"; break;
            case Json_Value::Type::Bool: out += v.boolean_value ? "true" : "false"; break;
            case Json_Value::Type::Number: {
                char buf[32];
                const double d = v.number_value;
                if (std::floor(d) == d && std::fabs(d) < 9e15) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(d));
                else std::snprintf(buf, sizeof buf, "%.17g", d);
                out += buf;
                break;
            }
            case Json_Value::Type::String: escape_into(v.string_value, out); break;
            case Json_Value::Type::Array:
                if (v.array_value.empty()) { out += "[]"; break; }
                out += "[\n";
                for (size_t i = 0; i < v.array_value.size(); ++i) {
                    out += inner;
                    dump(v.array_value[i], out, indent + 1);
                    if (i + 1 < v.array_value.size()) out.push_back(',');
                    out.push_back('\n');
                }
                out += pad + "]";
                break;
            case Json_Value::Type::Object: {
                if (v.object_value.empty()) { out += "{}"; break; }
                out += "{\n";
                size_t i = 0;
                for (const auto& [key, value] : v.object_value) {
                    out += inner;
                    escape_into(key, out);
                    out += ": ";
                    dump(value, out, indent + 1);
                    if (++i < v.object_value.size()) out.push_back(',');
                    out.push_back('\n');
                }
                out += pad + "}";
                break;
            }
        }
    }

    // Переводит один файл; возвращает размеры (json, ocb).
    std::pair<std::uintmax_t, std::uintmax_t> convert_one(const std::string& in, const std::string& out) {
        const Json_Value root = Json_Parser(read_text(in)).parse();
        Binary_Json::save_file(root, out);
        return {file_size(in), file_size(out)};
    }

    int cmd_convert(const std::string& in, std::string out) {
        if (out.empty()) out = fs::path(in).replace_extension(".ocb").string();
        const auto [json_size, bin_size] = convert_one(in, out);
        std::printf("%s -> %s  (%ju -> %ju bytes, %.1f%%)\n", in.c_str(), out.c_str(), json_size, bin_size,
                    json_size ? 100.0 * static_cast<double>(bin_size) / static_cast<double>(json_size) : 0.0);
        return 0;
    }

    int cmd_dir(const std::string& dir) {
        int count = 0;
        std::uintmax_t total_json = 0, total_bin = 0;
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            const std::string in = entry.path().string();
            const std::string out = fs::path(in).replace_extension(".ocb").string();
            const auto [json_size, bin_size] = convert_one(in, out);
            std::printf("%s -> %s  (%ju -> %ju bytes)\n", in.c_str(), out.c_str(), json_size, bin_size);
            total_json += json_size;
            total_bin += bin_size;
            ++count;
        }
        std::printf("converted %d files: %ju -> %ju bytes (%.1f%%)\n", count, total_json, total_bin,
                    total_json ? 100.0 * static_cast<double>(total_bin) / static_cast<double>(total_json) : 0.0);
        return 0;
    }

    int cmd_verify(const std::string& in) {
        const std::string text = read_text(in);
        const Json_Value from_text = Json_Parser(text).parse();
        const std::vector<uint8_t> bytes = Binary_Json::encode(from_text);
        const Json_Value from_bin = Binary_Json::decode(bytes);
        if (!deep_equal(from_text, from_bin)) {
            std::printf("FAIL: дерево после декодирования не совпадает с исходным (%s)\n", in.c_str());
            return 1;
        }

        constexpr int iterations = 200;
        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();
        for (int i = 0; i < iterations; ++i) {
            volatile size_t sink = Json_Parser(text).parse().object_value.size();
            (void)sink;
        }
        const double text_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count() / iterations;

        t0 = clock::now();
        for (int i = 0; i < iterations; ++i) {
            volatile size_t sink = Binary_Json::decode(bytes).object_value.size();
            (void)sink;
        }
        const double bin_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count() / iterations;

        std::printf("OK %s: json %zu bytes, ocjb %zu bytes (%.1f%%)\n", in.c_str(), text.size(), bytes.size(),
                    100.0 * static_cast<double>(bytes.size()) / static_cast<double>(text.size()));
        std::printf("    разбор текста: %.3f ms, декодирование OCJB: %.3f ms (x%.1f)\n",
                    text_ms, bin_ms, bin_ms > 0 ? text_ms / bin_ms : 0.0);
        return 0;
    }

    int cmd_dump(const std::string& in) {
        const Json_Value root = Binary_Json::load_file(in);
        std::string out;
        dump(root, out, 0);
        out.push_back('\n');
        std::fputs(out.c_str(), stdout);
        return 0;
    }
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::fprintf(stderr, "usage: json2bin <in.json> [out.ocb] | --dir <dir> | --verify <in.json> | --dump <in.ocb>\n");
            return 2;
        }
        const std::string first = argv[1];
        if (first == "--dir" && argc >= 3) return cmd_dir(argv[2]);
        if (first == "--verify" && argc >= 3) return cmd_verify(argv[2]);
        if (first == "--dump" && argc >= 3) return cmd_dump(argv[2]);
        return cmd_convert(first, argc >= 3 ? argv[2] : "");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "json2bin: %s\n", e.what());
        return 1;
    }
}
