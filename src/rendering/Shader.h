//
// Created by noktemor on 28.03.2026.
//

#ifndef OPTICRAFT_SHADER_H
#define OPTICRAFT_SHADER_H

#include <string>
#include <glm/glm.hpp>

class Shader {
private:
    unsigned int m_program_id;

    void check_compile_errors(unsigned int shader, const std::string& type);

public:
    Shader() : m_program_id(0) {}
    Shader(const std::string& vertex_path, const std::string& fragment_path);
    ~Shader();

    void use() const;

    // Uniform setters
    void set_bool(const std::string& name, bool value) const;
    void set_int(const std::string& name, int value) const;
    void set_float(const std::string& name, float value) const;
    void set_vec3(const std::string& name, const glm::vec3& value) const;
    void set_mat4(const std::string& name, const glm::mat4& value) const;

    unsigned int get_id() const { return m_program_id; }
};

#endif //OPTICRAFT_SHADER_H
