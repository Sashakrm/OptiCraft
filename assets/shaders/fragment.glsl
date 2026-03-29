#version 330 core
out vec4 FragColor;

in vec2 TexCoord;
in vec3 Normal;
in vec3 FragPos;

uniform sampler2D textureAtlas;

void main() {
    //  Простое освещение: яркость зависит от направления нормали
    float light_intensity = 0.7f;
    if (Normal.y > 0.9) {
        light_intensity = 1.0f;  // Верхняя грань — ярче
    } else if (Normal.y < -0.9) {
        light_intensity = 0.5f;  // Нижняя грань — темнее
    }

    //  Сэмплируем текстуру
    vec4 tex_color = texture(textureAtlas, TexCoord);

    //  Применяем освещение
    FragColor = vec4(tex_color.rgb * light_intensity, tex_color.a);
}