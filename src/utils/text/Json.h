//
// Минимальный самодостаточный JSON-парсер (без внешних зависимостей) — изначально жил
// только в Block_Types.cpp; вынесен сюда, чтобы Atlas_Registry (и любой будущий код,
// которому нужен JSON) мог использовать тот же парсер, а не заводить свою копию.
// Поддерживает object/array/string/number/bool/null — этого достаточно для конфигов
// проекта (blocks.json, *.atlas.json и т.п.), полноценным JSON5/комментариями не занимается.
//
#ifndef OPTICRAFT_JSON_H
#define OPTICRAFT_JSON_H

#include <string>
#include <unordered_map>
#include <vector>

struct Json_Value {
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Object,
        Array
    };

    Type type = Type::Null;
    bool boolean_value = false;
    double number_value = 0.0;
    std::string string_value;
    std::unordered_map<std::string, Json_Value> object_value;
    std::vector<Json_Value> array_value;

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }

    const Json_Value* find(const std::string& key) const {
        const auto it = object_value.find(key);
        return it == object_value.end() ? nullptr : &it->second;
    }
};

class Json_Parser {
public:
    explicit Json_Parser(std::string input) : m_input(std::move(input)) {}

    Json_Value parse() {
        skip_ws();
        Json_Value value = parse_value();
        skip_ws();
        return value;
    }

private:
    std::string m_input;
    size_t m_pos = 0;

    void skip_ws();
    char peek() const;
    char get();
    bool match(const std::string& token);
    void expect(char expected);

    Json_Value parse_value();
    Json_Value parse_object();
    Json_Value parse_array();
    Json_Value parse_string();
    Json_Value parse_number();
    Json_Value parse_true();
    Json_Value parse_false();
    Json_Value parse_null();
};

// Удобный хелпер: читает файл целиком и парсит. Бросает std::runtime_error на любую
// ошибку (файл не найден, некорректный JSON) — вызывающий код сам решает, ловить или нет.
Json_Value parse_json_file(const std::string& path);

// Мелкие строковые хелперы общего назначения — тоже были продублированы там, где нужен JSON.
std::string json_trim(const std::string& value);
std::string json_lower(std::string value);

// Типобезопасные геттеры с фолбэком — избавляют вызывающий код от ручных проверок is_number()/is_bool().
bool json_read_bool(const Json_Value& value, bool fallback);
int json_read_int(const Json_Value& value, int fallback);
float json_read_float(const Json_Value& value, float fallback);
std::string json_read_string(const Json_Value& value, const std::string& fallback = "");

#endif //OPTICRAFT_JSON_H
