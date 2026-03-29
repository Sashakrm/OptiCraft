//
// Created by noktemor on 28.03.2026.
//

#include "Shader.h"
#include "utils/Logger.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <glad/gl.h>

Shader::Shader(const std::string& vertex_path, const std::string& fragment_path) {
    std::string vertex_code;
    std::string fragment_code;
    std::ifstream v_shader_file;
    std::ifstream f_shader_file;

    v_shader_file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    f_shader_file.exceptions(std::ifstream::failbit | std::ifstream::badbit);

    try {
        v_shader_file.open(vertex_path);
        f_shader_file.open(fragment_path);

        std::stringstream v_shader_stream, f_shader_stream;
        v_shader_stream << v_shader_file.rdbuf();
        f_shader_stream << f_shader_file.rdbuf();

        vertex_code = v_shader_stream.str();
        fragment_code = f_shader_stream.str();

        v_shader_file.close();
        f_shader_file.close();
    } catch (std::ifstream::failure& e) {
        LOG_ERROR("Failed to read shader files: " + std::string(e.what()));
        return;
    }

    const char* v_shader_code = vertex_code.c_str();
    const char* f_shader_code = fragment_code.c_str();

    unsigned int vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &v_shader_code, nullptr);
    glCompileShader(vertex_shader);
    check_compile_errors(vertex_shader, "VERTEX");

    unsigned int fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &f_shader_code, nullptr);
    glCompileShader(fragment_shader);
    check_compile_errors(fragment_shader, "FRAGMENT");

    m_program_id = glCreateProgram();
    glAttachShader(m_program_id, vertex_shader);
    glAttachShader(m_program_id, fragment_shader);
    glLinkProgram(m_program_id);
    check_compile_errors(m_program_id, "PROGRAM");

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
}

Shader::~Shader() {
    if (m_program_id != 0) {
        glDeleteProgram(m_program_id);
    }
}

void Shader::use() const {
    glUseProgram(m_program_id);
}

void Shader::set_bool(const std::string& name, bool value) const {
    glUniform1i(glGetUniformLocation(m_program_id, name.c_str()), static_cast<int>(value));
    int loc = glGetUniformLocation(m_program_id, name.c_str());
    if (loc == -1) {
        std::cerr << "[WARN] Uniform '" << name << "' not found!\n";
    }
    glUniform1i(loc, value);
}

void Shader::set_int(const std::string& name, int value) const {
    glUniform1i(glGetUniformLocation(m_program_id, name.c_str()), value);
}

void Shader::set_float(const std::string& name, float value) const {
    glUniform1f(glGetUniformLocation(m_program_id, name.c_str()), value);
}

void Shader::set_vec3(const std::string& name, const glm::vec3& value) const {
    glUniform3fv(glGetUniformLocation(m_program_id, name.c_str()), 1, &value[0]);
}

void Shader::set_mat4(const std::string& name, const glm::mat4& value) const {
    glUniformMatrix4fv(glGetUniformLocation(m_program_id, name.c_str()), 1, GL_FALSE, &value[0][0]);
}

void Shader::check_compile_errors(unsigned int shader, const std::string& type) {
    int success;
    char info_log[1024];

    if (type != "PROGRAM") {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(shader, 1024, nullptr, info_log);
            LOG_ERROR("Shader compilation error (" + type + "):\n" + std::string(info_log));
        }
    } else {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(shader, 1024, nullptr, info_log);
            LOG_ERROR("Shader linking error:\n" + std::string(info_log));
        }
    }
}
