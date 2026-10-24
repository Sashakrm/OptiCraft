#include "utils/text/Json.h"
#include "utils/binary/Binary_Json.h"
#include "utils/logging/Logger.h"
#include <filesystem>
#include <string_view>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

void Json_Parser::skip_ws() {
    while (m_pos < m_input.size() && std::isspace(static_cast<unsigned char>(m_input[m_pos]))) {
        ++m_pos;
    }
}

char Json_Parser::peek() const {
    return m_pos < m_input.size() ? m_input[m_pos] : '\0';
}

char Json_Parser::get() {
    return m_pos < m_input.size() ? m_input[m_pos++] : '\0';
}

bool Json_Parser::match(const std::string& token) {
    if (m_input.compare(m_pos, token.size(), token) == 0) {
        m_pos += token.size();
        return true;
    }
    return false;
}

void Json_Parser::expect(char expected) {
    if (get() != expected) {
        throw std::runtime_error(std::string("JSON parse error: expected '") + expected + "'");
    }
}

Json_Value Json_Parser::parse_value() {
    skip_ws();
    const char c = peek();
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') return parse_string();
    if (c == 't') return parse_true();
    if (c == 'f') return parse_false();
    if (c == 'n') return parse_null();
    return parse_number();
}

Json_Value Json_Parser::parse_object() {
    Json_Value value;
    value.type = Json_Value::Type::Object;
    expect('{');
    skip_ws();
    if (peek() == '}') {
        get();
        return value;
    }

    while (true) {
        Json_Value key = parse_string();
        skip_ws();
        expect(':');
        Json_Value item = parse_value();
        value.object_value.emplace(std::move(key.string_value), std::move(item));
        skip_ws();
        const char c = get();
        if (c == '}') break;
        if (c != ',') throw std::runtime_error("JSON parse error: expected ',' or '}'");
        skip_ws();
    }

    return value;
}

Json_Value Json_Parser::parse_array() {
    Json_Value value;
    value.type = Json_Value::Type::Array;
    expect('[');
    skip_ws();
    if (peek() == ']') {
        get();
        return value;
    }

    while (true) {
        value.array_value.push_back(parse_value());
        skip_ws();
        const char c = get();
        if (c == ']') break;
        if (c != ',') throw std::runtime_error("JSON parse error: expected ',' or ']'");
        skip_ws();
    }

    return value;
}

Json_Value Json_Parser::parse_string() {
    Json_Value value;
    value.type = Json_Value::Type::String;
    expect('"');

    while (true) {
        if (m_pos >= m_input.size()) {
            throw std::runtime_error("JSON parse error: unterminated string");
        }
        char c = get();
        if (c == '"') break;
        if (c == '\\') {
            if (m_pos >= m_input.size()) {
                throw std::runtime_error("JSON parse error: bad escape");
            }
            const char escaped = get();
            switch (escaped) {
                case '"': value.string_value.push_back('"'); break;
                case '\\': value.string_value.push_back('\\'); break;
                case '/': value.string_value.push_back('/'); break;
                case 'b': value.string_value.push_back('\b'); break;
                case 'f': value.string_value.push_back('\f'); break;
                case 'n': value.string_value.push_back('\n'); break;
                case 'r': value.string_value.push_back('\r'); break;
                case 't': value.string_value.push_back('\t'); break;
                default:
                    throw std::runtime_error("JSON parse error: unsupported escape");
            }
        } else {
            value.string_value.push_back(c);
        }
    }

    return value;
}

Json_Value Json_Parser::parse_number() {
    Json_Value value;
    value.type = Json_Value::Type::Number;

    const size_t start = m_pos;
    if (peek() == '-') ++m_pos;
    while (std::isdigit(static_cast<unsigned char>(peek()))) ++m_pos;
    if (peek() == '.') {
        ++m_pos;
        while (std::isdigit(static_cast<unsigned char>(peek()))) ++m_pos;
    }
    if (peek() == 'e' || peek() == 'E') {
        ++m_pos;
        if (peek() == '+' || peek() == '-') ++m_pos;
        while (std::isdigit(static_cast<unsigned char>(peek()))) ++m_pos;
    }

    const std::string token = m_input.substr(start, m_pos - start);
    value.number_value = std::stod(token);
    return value;
}

Json_Value Json_Parser::parse_true() {
    if (!match("true")) throw std::runtime_error("JSON parse error: expected true");
    Json_Value value;
    value.type = Json_Value::Type::Bool;
    value.boolean_value = true;
    return value;
}

Json_Value Json_Parser::parse_false() {
    if (!match("false")) throw std::runtime_error("JSON parse error: expected false");
    Json_Value value;
    value.type = Json_Value::Type::Bool;
    value.boolean_value = false;
    return value;
}

Json_Value Json_Parser::parse_null() {
    if (!match("null")) throw std::runtime_error("JSON parse error: expected null");
    return {};
}

namespace {
    // Путь бинарной версии: x.json -> x.ocb. Пусто, если путь не *.json.
    std::string binary_sibling(const std::string& json_path) {
        constexpr std::string_view k_ext = ".json";
        if (json_path.size() < k_ext.size() ||
            json_path.compare(json_path.size() - k_ext.size(), k_ext.size(), k_ext) != 0) {
            return {};
        }
        return json_path.substr(0, json_path.size() - k_ext.size()) + ".ocb";
    }

    // Бинарный файл берём, если он есть и не старее JSON. Так отредактированный JSON не
    // перекрывается устаревшим .ocb.
    bool binary_is_fresh(const std::string& json_path, const std::string& bin_path) {
        std::error_code ec;
        if (!std::filesystem::exists(bin_path, ec)) return false;
        if (!std::filesystem::exists(json_path, ec)) return true;
        return std::filesystem::last_write_time(bin_path, ec) >= std::filesystem::last_write_time(json_path, ec);
    }
} // namespace

bool json_asset_exists(const std::string& json_path) {
    std::error_code ec;
    if (std::filesystem::exists(json_path, ec)) return true;
    const std::string bin = binary_sibling(json_path);
    return !bin.empty() && std::filesystem::exists(bin, ec);
}

Json_Value parse_json_file(const std::string& path) {
    const std::string bin = binary_sibling(path);
    if (!bin.empty() && binary_is_fresh(path, bin)) {
        try {
            return Binary_Json::load_file(bin);
        } catch (const std::exception& e) {
            // Битый бинарник не должен ломать загрузку: откатываемся на исходный JSON.
            Logger::warning("binary asset rejected, using JSON: " + bin + " (" + e.what() + ")");
        }
    }
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open file: " + path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    Json_Parser parser(buffer.str());
    return parser.parse();
}

std::string json_trim(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string json_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool json_read_bool(const Json_Value& value, bool fallback) {
    if (value.is_bool()) return value.boolean_value;
    if (value.is_number()) return value.number_value != 0.0;
    return fallback;
}

int json_read_int(const Json_Value& value, int fallback) {
    if (value.is_number()) return static_cast<int>(std::lround(value.number_value));
    return fallback;
}

float json_read_float(const Json_Value& value, float fallback) {
    if (value.is_number()) return static_cast<float>(value.number_value);
    return fallback;
}

std::string json_read_string(const Json_Value& value, const std::string& fallback) {
    if (value.is_string()) return value.string_value;
    return fallback;
}
