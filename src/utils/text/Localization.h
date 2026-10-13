#ifndef OPTICRAFT_LOCALIZATION_H
#define OPTICRAFT_LOCALIZATION_H

#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
//  Строки интерфейса
// ----------------------------------------------------------------------------
//  Все тексты UI лежат в assets/lang/<код>.json ("en", "ru", ...) как плоский словарь
//  "ключ": "текст" в UTF-8. В коде тексты берутся через tr("menu.singleplayer").
//  Шаблоны с подстановкой используют {0}, {1}...: tr("options.gui_scale", {"2x"}).
//  Английский (en) всегда загружается как запасной: если ключа нет в выбранном языке,
//  берётся английская строка, а если нет и её — возвращается сам ключ (виден в UI сразу).
// ============================================================================
class Localization {
public:
    static Localization& get();

    // Загружает язык (с запасным en). false — файл языка не найден/битый, остаётся прежний.
    bool set_language(const std::string& code);
    const std::string& get_language() const { return m_language; }
    // Коды языков, для которых есть файл в assets/lang.
    const std::vector<std::string>& get_available_languages() const { return m_available; }
    // Человекочитаемое имя языка (ключ "language.name" из его же файла).
    std::string get_language_display_name(const std::string& code) const;

    std::string translate(const std::string& key, const std::vector<std::string>& args = {}) const;

private:
    Localization();
    static bool load_file(const std::string& code, std::unordered_map<std::string, std::string>& out);

    std::string m_language = "en";
    std::unordered_map<std::string, std::string> m_strings;
    std::unordered_map<std::string, std::string> m_fallback;
    std::vector<std::string> m_available;
};

inline std::string tr(const std::string& key, const std::vector<std::string>& args = {}) {
    return Localization::get().translate(key, args);
}

#endif
