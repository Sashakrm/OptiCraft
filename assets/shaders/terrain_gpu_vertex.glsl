#version 430 core
// Вершинный шейдер GPU-мешера (см. Renderer::draw_solid_gpu). Квад не хранится как шесть Terrain_Vertex:
// каждая вершина разворачивается из 16-байтной записи квада (mesh_greedy.comp) прямо здесь.
// Выходы совпадают с terrain_vertex.glsl, поэтому фрагментный шейдер общий по смыслу.
//
// Вызов: glMultiDrawArraysIndirect, first = номер первого квада секции в пуле * 6, baseInstance =
// ручка чанка*4 + секция. gl_BaseInstance в GLSL 4.30 нет, поэтому номер приходит атрибутом aSection
// с делителем 1: при baseInstance GL читает элемент с индексом baseInstance + 0, а буфер
// содержит тождественную последовательность 0..N-1. gl_VertexID у DrawArrays начинается с first,
// поэтому квад = gl_VertexID / 6 — абсолютный номер в пуле, угол = gl_VertexID % 6.
layout(location = 0) in uint aSection;
layout(std430, binding = 2) readonly buffer Quads { uvec4 quads[]; };
layout(std430, binding = 8) readonly buffer Rects { vec4 rects[]; };
// Мировой минимальный угол каждого чанка по ручке (xyz); w не используется.
layout(std430, binding = 9) readonly buffer Origins { vec4 origins[]; };

// Готовая projection * view (model здесь всегда единичная): одно умножение на вершину вместо трёх.
uniform mat4 view_projection;

out vec2 Local;
out float NormalY;
out float BlockLight;
out float SkyLight;
flat out vec4 Rect;
out float AO;

// Углы квада (fu + 2*fv) по шести вершинам, в порядке emit_terrain_quad (Chunk.cpp), включая
// переворот порядка при знаке. Вариант = axis*2 + (знак > 0 ? 0 : 1).
const int kCorner[36] = int[36](
    0, 2, 3, 0, 3, 1,
    0, 3, 2, 0, 1, 3,
    0, 3, 1, 0, 2, 3,
    0, 1, 3, 0, 3, 2,
    0, 1, 3, 0, 3, 2,
    0, 3, 1, 0, 2, 3);

// Та же таблица для квада, разрезанного по диагонали c1-c2 (см. flip_quad_diagonal в Chunk.cpp).
const int kCornerFlip[36] = int[36](
    0, 2, 1, 1, 2, 3,
    0, 1, 2, 1, 3, 2,
    0, 2, 1, 1, 2, 3,
    0, 1, 2, 1, 3, 2,
    0, 1, 2, 1, 3, 2,
    0, 2, 1, 1, 2, 3);

void main() {
    const uint vid = uint(gl_VertexID);
    const uint qi = vid / 6u;
    const uint k = vid - qi * 6u;
    const uint sec = aSection;

    const uvec4 rec = quads[qi];
    const uint w0 = rec.x;
    const uint w1 = rec.y;
    const uint w2 = rec.z;   // свет четырёх углов: байт угла = sky<<4 | blk
    const vec3 cell = vec3(float(w0 & 31u), float((w0 >> 5u) & 31u), float((w0 >> 10u) & 31u));
    const uint axis = (w0 >> 15u) & 3u;
    const bool pos = ((w0 >> 17u) & 1u) != 0u;
    const float w = float(((w0 >> 18u) & 31u) + 1u);
    const float h = float(((w0 >> 23u) & 31u) + 1u);
    const bool rot = ((w0 >> 28u) & 1u) != 0u;
    const float normal_extent = float(1u << ((w0 >> 29u) & 3u));

    // AO углов: два бита на угол (код fu + 2*fv), см. face_ao в Chunk.cpp. Диагональ идёт через
    // более тёмную пару углов, поэтому порядок вершин выбирается по AO.
    const uint ao_bits = (w1 >> 24u) & 0xFFu;
    const uint a0 = ao_bits & 3u;
    const uint a1 = (ao_bits >> 2u) & 3u;
    const uint a2 = (ao_bits >> 4u) & 3u;
    const uint a3 = (ao_bits >> 6u) & 3u;
    const bool flip = a0 + a3 > a1 + a2;
    const uint variant = axis * 2u + (pos ? 0u : 1u);
    const int code = flip ? kCornerFlip[variant * 6u + k] : kCorner[variant * 6u + k];
    const float fu = float(code & 1);
    const float fv = float((code >> 1) & 1);
    const vec3 base = origins[sec >> 2u].xyz + cell;
    const float plane = pos ? normal_extent : 0.0;

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
    const uint cl = (w2 >> (8u * uint(code))) & 0xFFu;
    SkyLight = float(cl >> 4u);
    BlockLight = float(cl & 15u);
    Rect = rects[w1 & 0x7FFFu];
    AO = float((ao_bits >> (2u * uint(code))) & 3u);
    gl_Position = view_projection * vec4(p, 1.0);
}
