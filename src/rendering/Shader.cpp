//
// Created by noktemor on 28.03.2026.
//

#include "Shader.h"
#include "utils/Logger.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <glad/gl.h>

// === Вспомогательная: чтение файла ===
static std::string read_file(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open file: " + std::string(path));
        return "";
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// === Конструктор: загрузка из файлов ===
Shader::Shader(const char* vertex_path, const char* fragment_path) {
    std::string vertex_code = read_file(vertex_path);
    std::string fragment_code = read_file(fragment_path);

    if (vertex_code.empty() || fragment_code.empty()) {
        LOG_ERROR("Failed to read shader files");
        m_program_id = 0;
        return;
    }

    compile(vertex_code.c_str(), fragment_code.c_str());
}

// === Конструктор: загрузка из строк ===
Shader::Shader(const std::string& vertex_src, const std::string& fragment_src) {
    compile(vertex_src.c_str(), fragment_src.c_str());
}

// === Компиляция шейдеров ===
void Shader::compile(const char* vertex_src, const char* fragment_src) {
    // Vertex shader
    unsigned int vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_src, nullptr);
    glCompileShader(vertex_shader);

    // Проверка компиляции
    int success;
    char info_log[512];
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertex_shader, 512, nullptr, info_log);
        LOG_ERROR("Vertex shader compilation failed: " + std::string(info_log));
    }

    // Fragment shader
    unsigned int fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_src, nullptr);
    glCompileShader(fragment_shader);

    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragment_shader, 512, nullptr, info_log);
        LOG_ERROR("Fragment shader compilation failed: " + std::string(info_log));
    }

    // Program
    m_program_id = glCreateProgram();
    glAttachShader(m_program_id, vertex_shader);
    glAttachShader(m_program_id, fragment_shader);
    glLinkProgram(m_program_id);

    glGetProgramiv(m_program_id, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramInfoLog(m_program_id, 512, nullptr, info_log);
        LOG_ERROR("Shader program linking failed: " + std::string(info_log));
    }

    // Cleanup
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
}

// === Активировать шейдер ===
void Shader::use() {
    glUseProgram(m_program_id);
}

// === Сеттеры униформов ===
void Shader::set_bool(const std::string& name, bool value) {
    glUniform1i(glGetUniformLocation(m_program_id, name.c_str()), static_cast<int>(value));
}

void Shader::set_int(const std::string& name, int value) {
    glUniform1i(glGetUniformLocation(m_program_id, name.c_str()), value);
}

void Shader::set_float(const std::string& name, float value) {
    glUniform1f(glGetUniformLocation(m_program_id, name.c_str()), value);
}

void Shader::set_vec2(const std::string& name, const glm::vec2& value) {
    glUniform2fv(glGetUniformLocation(m_program_id, name.c_str()), 1, &value[0]);
}

void Shader::set_vec3(const std::string& name, const glm::vec3& value) {
    glUniform3fv(glGetUniformLocation(m_program_id, name.c_str()), 1, &value[0]);
}

void Shader::set_vec4(const std::string& name, const glm::vec4& value) {
    glUniform4fv(glGetUniformLocation(m_program_id, name.c_str()), 1, &value[0]);
}

void Shader::set_mat4(const std::string& name, const glm::mat4& value) {
    glUniformMatrix4fv(glGetUniformLocation(m_program_id, name.c_str()), 1, GL_FALSE, &value[0][0]);
}

// === Геттер ID программы (для проверки) ===
/*unsigned int Shader::get_id() const {
    return m_program_id;
}*/

// === Деструктор ===
Shader::~Shader() {
    if (m_program_id > 0) {
        glDeleteProgram(m_program_id);
    }
}