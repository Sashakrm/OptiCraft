//
// Created by noktemor on 29.03.2026.
//

#include "Texture.h"
#include "Logger.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

Texture::Texture()
    : m_texture_id(0)
    , m_width(0)
    , m_height(0)
    , m_channels(0)
    , m_is_loaded(false)
{}

Texture::~Texture() {
    if (m_texture_id != 0) {
        glDeleteTextures(1, &m_texture_id);
    }
}

bool Texture::load_from_file(const std::string& path) {
    stbi_set_flip_vertically_on_load(true);

    unsigned char* data = stbi_load(path.c_str(), &m_width, &m_height, &m_channels, 4);
    if (!data) {
        LOG_ERROR("Failed to load textures: " + path + " - " + stbi_failure_reason());
        return false;
    }

    glGenTextures(1, &m_texture_id);
    glBindTexture(GL_TEXTURE_2D, m_texture_id);

    // Настройки фильтрации
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Настройки обёртки (повторение)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Загрузка данных (RGBA)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);

    m_is_loaded = true;
    LOG_INFO("Texture loaded: " + path + " (" + std::to_string(m_width) + "x" + std::to_string(m_height) + ")");
    return true;
}

void Texture::bind(unsigned int texture_unit) const {
    if (!m_is_loaded) return;
    glActiveTexture(GL_TEXTURE0 + texture_unit);
    glBindTexture(GL_TEXTURE_2D, m_texture_id);
}

void Texture::unbind() const {
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::set_wrap_mode(int wrap_s, int wrap_t) {
    if (!m_is_loaded) return;
    glBindTexture(GL_TEXTURE_2D, m_texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture::set_filter_mode(int min_filter, int mag_filter) {
    if (!m_is_loaded) return;
    glBindTexture(GL_TEXTURE_2D, m_texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
    glBindTexture(GL_TEXTURE_2D, 0);
}