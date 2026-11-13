#version 430 core
// Облегчённый вершинный шейдер предпрохода глубины (Hi-Z). Геометрия и порядок вершин те же, что в
// terrain_gpu_vertex.glsl, но без всего, что нужно только для цвета: UV, свет, AO, объём света, Rect.
// Предпроход пишет только глубину (цвет отключён), поэтому полный шейдер terrain тратил на нём
// впустую около дюжины чтений SSBO на фрагмент плюс выборку текстуры.
//
// Триангуляция берётся без учёта AO (kCorner, без kCornerFlip): плоский квад покрывает те же пиксели
// при любой диагонали, а обход вершин (лицевая сторона) у обеих таблиц одинаковый.
layout(location = 0) in uint aSection;
layout(std430, binding = 2) readonly buffer Quads { uvec4 quads[]; };
layout(std430, binding = 9) readonly buffer Origins { vec4 origins[]; };

uniform mat4 view_projection;
uniform float time;
uniform float appearDuration;

out vec3 WorldPos;
flat out vec3 WorldNormal;
flat out float Appearance;

const int kCorner[36] = int[36](
    0, 2, 3, 0, 3, 1,
    0, 3, 2, 0, 1, 3,
    0, 3, 1, 0, 2, 3,
    0, 1, 3, 0, 3, 2,
    0, 1, 3, 0, 3, 2,
    0, 3, 1, 0, 2, 3);

void main() {
    const uint vid = uint(gl_VertexID);
    const uint qi = vid / 6u;
    const uint k = vid - qi * 6u;
    const uint sec = aSection;

    const uvec4 rec = quads[qi];
    const uint w0 = rec.x;
    const vec3 cell = vec3(float(w0 & 31u), float((w0 >> 5u) & 31u), float((w0 >> 10u) & 31u));
    const uint axis = (w0 >> 15u) & 3u;
    const bool pos = ((w0 >> 17u) & 1u) != 0u;
    const float w = float(((w0 >> 18u) & 31u) + 1u);
    const float h = float(((w0 >> 23u) & 31u) + 1u);
    const float normal_extent = float(1u << ((w0 >> 29u) & 3u));

    const uint variant = axis * 2u + (pos ? 0u : 1u);
    const int code = kCorner[variant * 6u + k];
    const float fu = float(code & 1);
    const float fv = float((code >> 1) & 1);
    const vec4 chunk_origin = origins[sec >> 2u];
    const vec3 base = chunk_origin.xyz + cell;
    const float plane = pos ? normal_extent : 0.0;

    vec3 p;
    vec3 world_normal = vec3(0.0);
    if (axis == 0u) {
        p = vec3(base.x + plane, base.y + fv * h, base.z + fu * w);
        world_normal.x = pos ? 1.0 : -1.0;
    } else if (axis == 1u) {
        p = vec3(base.x + fu * w, base.y + plane, base.z + fv * h);
        world_normal.y = pos ? 1.0 : -1.0;
    } else {
        p = vec3(base.x + fu * w, base.y + fv * h, base.z + plane);
        world_normal.z = pos ? 1.0 : -1.0;
    }

    // Та же формула появления чанка, что в terrain_gpu_vertex.glsl: растворяющиеся фрагменты
    // не должны писать глубину, иначе Hi-Z скроет геометрию, видимую сквозь них.
    const float appear = chunk_origin.w < 0.0 ? 1.0
        : clamp((time - chunk_origin.w) / max(appearDuration, 0.05), 0.0, 1.0);
    Appearance = appear * appear * (3.0 - 2.0 * appear);
    WorldPos = p;
    WorldNormal = world_normal;
    gl_Position = view_projection * vec4(p, 1.0);
}
