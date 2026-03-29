//
// Created by noktemor on 29.03.2026.
//

#ifndef OPTICRAFT_TEXTURE_H
#define OPTICRAFT_TEXTURE_H

#include <glad/gl.h>
#include <string>

class Texture {
private:
    unsigned int m_texture_id;
    int m_width;
    int m_height;
    int m_channels;
    bool m_is_loaded;

public:
    Texture();
    ~Texture();

    // Запрет копирования
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // Загрузка из файла
    bool load_from_file(const std::string& path);

    // Привязка к текстурному юниту
    void bind(unsigned int texture_unit = 0) const;
    void unbind() const;

    // Геттеры
    unsigned int get_id() const { return m_texture_id; }
    int get_width() const { return m_width; }
    int get_height() const { return m_height; }
    bool is_loaded() const { return m_is_loaded; }

    // Настройки
    void set_wrap_mode(int wrap_s, int wrap_t);
    void set_filter_mode(int min_filter, int mag_filter);
};

#endif //OPTICRAFT_TEXTURE_H
