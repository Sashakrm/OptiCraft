#version 330 core
in vec2 vLocalPos;
out vec4 FragColor;

uniform vec3 color;
uniform float alpha; // общая непрозрачность диска (для мягкого исчезновения под горизонтом)
// Фаза освещения диска: -1 — новолуние, 0 — половина, 1 — полнолуние. Значение > 1 — фаза не
// рисуется (диск целиком освещён).
uniform float illumination;
// Чёрное ядро: радиус в долях радиуса диска. 0 — нет ядра, 1 — весь диск чёрный.
uniform float core;

void main() {
    const float softness = 0.08;
    float dist = length(vLocalPos);
    float edge = 1.0 - smoothstep(1.0 - softness, 1.0, dist);
    if (edge <= 0.0 || alpha <= 0.0) {
        discard;
    }

    // Терминатор — полуэллипс: x > -illumination * sqrt(1 - y^2).
    float lit = 1.0;
    if (illumination <= 1.0) {
        float terminator = -illumination * sqrt(max(0.0, 1.0 - vLocalPos.y * vLocalPos.y));
        lit = smoothstep(terminator - 0.03, terminator + 0.03, vLocalPos.x);
    }
    float shade = mix(0.12, 1.0, lit);
    float opacity = mix(0.15, 1.0, lit);
    vec3 base_color = color * shade;
    float base_alpha = edge * alpha * opacity;

    // Чёрное ядро поверх диска: непрозрачное, с мягким краем.
    float core_mask = 0.0;
    if (core > 0.0) {
        core_mask = 1.0 - smoothstep(core - 0.02, core + 0.02, dist);
    }
    FragColor = vec4(mix(base_color, vec3(0.0), core_mask), mix(base_alpha, edge * alpha, core_mask));
}
