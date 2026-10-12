#version 430 core
// Вершинный шейдер GPU-мешера (см. Renderer::draw_solid_gpu). Квад не хранится как шесть Terrain_Vertex:
// каждая вершина разворачивается из 8-байтной записи квада (mesh_greedy.comp) прямо здесь.
// Выходы совпадают с terrain_vertex.glsl, поэтому фрагментный шейдер общий по смыслу.
//
// Вызов: glMultiDrawArraysIndirect, first = 0, baseInstance = глобальный номер секции
// (слот*4 + секция). gl_BaseInstance в GLSL 4.30 нет, поэтому номер приходит атрибутом aSection
// с делителем 1: при baseInstance GL читает элемент с индексом baseInstance + 0, а буфер
// содержит тождественную последовательность 0..N-1. Вершина: квад = gl_VertexID / 6, угол = %6.
layout(location = 0) in uint aSection;
layout(std430, binding = 2) readonly buffer Quads { uvec2 quads[]; };
layout(std430, binding = 8) readonly buffer Rects { vec4 rects[]; };
// Мировой минимальный угол каждого слота (xyz); w не используется.
layout(std430, binding = 9) readonly buffer Origins { vec4 origins[]; };

uniform int u_quad_cap;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec2 Local;
out float NormalY;
out float BlockLight;
flat out vec4 Rect;

// Углы квада (fu + 2*fv) по шести вершинам, в порядке emit_terrain_quad (Chunk.cpp), включая
// переворот порядка при знаке. Вариант = axis*2 + (знак > 0 ? 0 : 1).
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

    const uvec2 rec = quads[sec * uint(u_quad_cap) + qi];
    const uint w0 = rec.x;
    const uint w1 = rec.y;
    const vec3 cell = vec3(float(w0 & 31u), float((w0 >> 5u) & 31u), float((w0 >> 10u) & 31u));
    const uint axis = (w0 >> 15u) & 3u;
    const bool pos = ((w0 >> 17u) & 1u) != 0u;
    const float w = float(((w0 >> 18u) & 31u) + 1u);
    const float h = float(((w0 >> 23u) & 31u) + 1u);
    const bool rot = ((w0 >> 28u) & 1u) != 0u;

    const int code = kCorner[(axis * 2u + (pos ? 0u : 1u)) * 6u + k];
    const float fu = float(code & 1);
    const float fv = float((code >> 1) & 1);
    const vec3 base = origins[sec >> 2u].xyz + cell;
    const float plane = pos ? 1.0 : 0.0;

    vec3 p;
    vec2 local;
    if (axis == 0u) {
        p = vec3(base.x + plane, base.y + fv * h, base.z + fu * w);
        local = vec2(fu * w, fv * h);
    } else if (axis == 1u) {
        p = vec3(base.x + fu * w, base.y + plane, base.z + fv * h);
        local = vec2(fu * w, (1.0 - fv) * h);
    } else {
        p = vec3(base.x + fu * w, base.y + fv * h, base.z + plane);
        local = vec2(fu * w, fv * h);
    }
    // Повёрнутая текстура (бревно на боку): (a, b) -> (b, 1 - a), как rotate_quarter в Chunk.cpp.
    if (rot) local = vec2(local.y, 1.0 - local.x);

    Local = local;
    NormalY = (axis == 1u) ? (pos ? 1.0 : -1.0) : 0.0;
    BlockLight = float((w1 >> 16u) & 0xFFu);
    Rect = rects[w1 & 0x7FFFu];
    gl_Position = projection * view * model * vec4(p, 1.0);
}
