#ifndef OPTICRAFT_BINARY_JSON_H
#define OPTICRAFT_BINARY_JSON_H

// Бинарный формат OCJB (OptiCraft JSON Binary) — то же дерево Json_Value, что и у текстового
// парсера, но без токенизации текста на загрузке и с компактным хранением:
//
//   заголовок (20 байт): "OCJB" | u16 версия (1) | u16 резерв (0) | u32 контрольная сумма |
//                        u32 число строк | u32 размер полезной нагрузки
//   таблица строк: для каждой строки varint длина + байты (каждая строка хранится один раз)
//   дерево значений: байт-тег, далее данные
//       0 null, 1 false, 2 true, 3 целое (zigzag varint), 4 double (8 байт LE),
//       5 строка (varint индекс в таблице), 6 массив (varint число, значения),
//       7 объект (varint число, пары: varint индекс ключа + значение)
//
// Контрольная сумма — FNV-1a по полезной нагрузке (всё после заголовка). Файл, который не
// проходит проверку (неверный magic, версия, сумма, выход за границы), отвергается исключением.

#include <cstdint>
#include <string>
#include <vector>
#include "utils/text/Json.h"

namespace Binary_Json {
    inline constexpr uint16_t k_version = 1;

    std::vector<uint8_t> encode(const Json_Value& root);
    // Бросает std::runtime_error на любую ошибку формата.
    Json_Value decode(const std::vector<uint8_t>& bytes);

    void save_file(const Json_Value& root, const std::string& path);
    Json_Value load_file(const std::string& path);
}

#endif //OPTICRAFT_BINARY_JSON_H
