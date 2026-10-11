#ifndef OPTICRAFT_UTF8_H
#define OPTICRAFT_UTF8_H

#include <cstddef>
#include <string>

// Минимальные помощники для UTF-8 — всё, что нужно полю ввода и отрисовке текста.
namespace Utf8 {

    // Декодирует символ, начинающийся в text[i], и сдвигает i за него.
    // Битые последовательности отдают U+FFFD и съедают один байт.
    inline char32_t next(const std::string& text, size_t& i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        int extra = 0;
        char32_t cp = 0;
        if (c < 0x80) { ++i; return c; }
        if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else { ++i; return 0xFFFD; }
        if (i + static_cast<size_t>(extra) >= text.size()) { ++i; return 0xFFFD; }
        for (int k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(text[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) { ++i; return 0xFFFD; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += static_cast<size_t>(extra) + 1;
        return cp;
    }

    inline void append(std::string& out, char32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // Удаляет последний символ целиком (а не один байт).
    inline void pop_back(std::string& text) {
        if (text.empty()) return;
        size_t i = text.size() - 1;
        while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) --i;
        text.erase(i);
    }

    // Число символов (кодовых точек).
    inline size_t length(const std::string& text) {
        size_t count = 0;
        for (const char c : text) {
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++count;
        }
        return count;
    }

    // Первые count символов строки.
    inline std::string prefix(const std::string& text, size_t count) {
        size_t i = 0;
        for (size_t n = 0; n < count && i < text.size(); ++n) next(text, i);
        return text.substr(0, i);
    }

    // Последние count символов строки.
    inline std::string suffix(const std::string& text, size_t count) {
        const size_t total = length(text);
        if (total <= count) return text;
        size_t i = 0;
        for (size_t n = 0; n < total - count; ++n) next(text, i);
        return text.substr(i);
    }

    // Символы, которые умеет рисовать шрифт HUD: печатный ASCII и русская кириллица.
    inline bool is_supported_by_font(char32_t cp) {
        return (cp >= 32 && cp < 127) || (cp >= 0x410 && cp <= 0x44F) || cp == 0x401 || cp == 0x451;
    }

} // namespace Utf8

#endif
