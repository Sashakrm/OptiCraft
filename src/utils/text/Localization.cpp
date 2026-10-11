#include "utils/text/Localization.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include "utils/text/Json.h"
#include "utils/logging/Logger.h"

namespace {
    constexpr const char* k_lang_dir = "assets/lang/";
    constexpr const char* k_fallback_language = "en";
}

Localization& Localization::get() {
    static Localization instance;
    return instance;
}

Localization::Localization() {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(k_lang_dir, error)) {
        if (entry.path().extension() == ".json") m_available.push_back(entry.path().stem().string());
    }
    std::sort(m_available.begin(), m_available.end());
    // "en" всегда первый, чтобы переключение языка шло en → остальные по алфавиту.
    auto en = std::find(m_available.begin(), m_available.end(), k_fallback_language);
    if (en != m_available.end()) std::rotate(m_available.begin(), en, en + 1);

    load_file(k_fallback_language, m_fallback);
    m_strings = m_fallback;
}

bool Localization::load_file(const std::string& code, std::unordered_map<std::string, std::string>& out) {
    try {
        const Json_Value root = parse_json_file(std::string(k_lang_dir) + code + ".json");
        if (!root.is_object()) throw std::runtime_error("root is not an object");
        out.clear();
        for (const auto& [key, value] : root.object_value) {
            if (value.is_string()) out[key] = value.string_value;
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Localization: cannot load \"" + code + "\": " + e.what());
        return false;
    }
}

bool Localization::set_language(const std::string& code) {
    std::unordered_map<std::string, std::string> loaded;
    if (!load_file(code, loaded)) return false;
    m_strings = std::move(loaded);
    m_language = code;
    return true;
}

std::string Localization::get_language_display_name(const std::string& code) const {
    if (code == m_language) {
        const auto it = m_strings.find("language.name");
        if (it != m_strings.end()) return it->second;
    }
    std::unordered_map<std::string, std::string> tmp;
    if (load_file(code, tmp)) {
        const auto it = tmp.find("language.name");
        if (it != tmp.end()) return it->second;
    }
    return code;
}

std::string Localization::translate(const std::string& key, const std::vector<std::string>& args) const {
    const std::string* text = nullptr;
    if (auto it = m_strings.find(key); it != m_strings.end()) text = &it->second;
    else if (auto fb = m_fallback.find(key); fb != m_fallback.end()) text = &fb->second;
    if (!text) return key;
    if (args.empty()) return *text;

    std::string result;
    result.reserve(text->size() + 16);
    for (size_t i = 0; i < text->size(); ++i) {
        if ((*text)[i] == '{') {
            const size_t close = text->find('}', i);
            if (close != std::string::npos && close > i + 1) {
                const std::string index_str = text->substr(i + 1, close - i - 1);
                if (std::all_of(index_str.begin(), index_str.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    const size_t index = static_cast<size_t>(std::stoul(index_str));
                    if (index < args.size()) result += args[index];
                    i = close;
                    continue;
                }
            }
        }
        result += (*text)[i];
    }
    return result;
}
