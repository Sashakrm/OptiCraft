//
// Created by noktemor on 28.03.2026.
//

#include "rendering/Shader.h"
#include "utils/logging/Logger.h"
#include <fstream>
#include <sstream>
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
    const int loc = get_uniform_location(name);
    if (loc == -1) {
        LOG_WARN("Uniform '" + name + "' not found");
        return;
    }
    glUniform1i(loc, static_cast<int>(value));
}

void Shader::set_int(const std::string& name, int value) const {
    glUniform1i(get_uniform_location(name), value);
}

void Shader::set_float(const std::string& name, float value) const {
    glUniform1f(get_uniform_location(name), value);
}

void Shader::set_vec2(const std::string& name, const glm::vec2& value) const {
    glUniform2fv(get_uniform_location(name), 1, &value[0]);
}

void Shader::set_vec3(const std::string& name, const glm::vec3& value) const {
    glUniform3fv(get_uniform_location(name), 1, &value[0]);
}

void Shader::set_vec4(const std::string& name, const glm::vec4& value) const {
    glUniform4fv(get_uniform_location(name), 1, &value[0]);
}

void Shader::set_mat4(const std::string& name, const glm::mat4& value) const {
    glUniformMatrix4fv(get_uniform_location(name), 1, GL_FALSE, &value[0][0]);
}

void Shader::set_mat4_array(const std::string& name, const std::vector<glm::mat4>& values) const {
    if (values.empty()) return;
    glUniformMatrix4fv(get_uniform_location(name), static_cast<GLsizei>(values.size()), GL_FALSE, &values[0][0][0]);
}

int Shader::get_uniform_location(const std::string& name) const {
    if (const auto it = m_uniform_location_cache.find(name); it != m_uniform_location_cache.end()) {
        return it->second;
    }

    const int location = glGetUniformLocation(m_program_id, name.c_str());
    m_uniform_location_cache.emplace(name, location);
    return location;
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
