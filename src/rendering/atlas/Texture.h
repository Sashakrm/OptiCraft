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
    // flip_vertically = true — для блочного атласа и HUD (ось V у них направлена вверх, см. комментарии
    // в Chunk.cpp/Texture_Atlas.cpp). glTF-модели (мобы) требуют false: у них V идёт СВЕРХУ вниз, и с
    // переворотом UV сэмплировали бы зеркальную по вертикали часть картинки.
    // nearest = true — фильтр NEAREST вместо LINEAR (пиксель-арт без размытия).
    bool load_from_file(const std::string& path, bool flip_vertically = true, bool nearest = true);

    // Загрузка из буфера в памяти (нужно для текстур, встроенных в .glb —
    // там картинка лежит как кусок бинарных PNG/JPEG-байт внутри файла,
    // а не как отдельный файл на диске). debug_name — только для логов.
    bool load_from_memory(const unsigned char* data, int data_size, const std::string& debug_name = "<memory>",
                          bool flip_vertically = true, bool nearest = false);

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
