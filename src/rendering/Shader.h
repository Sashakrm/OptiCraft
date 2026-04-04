//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_SHADER_H
#define OPTICRAFT_SHADER_H

#include <string>
#include <glm/glm.hpp>
#include <glad/gl.h>

class Shader {
private:
    unsigned int m_program_id;

    void compile(const char* vertex_src, const char* fragment_src);

public:
    // Конструкторы
    Shader() : m_program_id(0) {}
    Shader(const char* vertex_path, const char* fragment_path);
    Shader(const std::string& vertex_src, const std::string& fragment_src);

    ~Shader();

    // Активация
    void use();

    // Униформы
    void set_bool(const std::string& name, bool value);
    void set_int(const std::string& name, int value);
    void set_float(const std::string& name, float value);
    void set_vec2(const std::string& name, const glm::vec2& value);
    void set_vec3(const std::string& name, const glm::vec3& value);
    void set_vec4(const std::string& name, const glm::vec4& value);
    void set_mat4(const std::string& name, const glm::mat4& value);

    // Утилита
    unsigned int get_id() const { return m_program_id; }
};

#endif //OPTICRAFT_SHADER_H
