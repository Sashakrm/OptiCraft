#version 430 core
// Вершинный шейдер GPU-мешера для блоков формы, флоры и жидкости (записи из mesh_misc.comp).
// Раскладка записи и порядок граней описаны в mesh_misc.comp. Вызов — как у terrain_gpu_vertex.glsl:
// first = номер первого квада потока * 6, baseInstance = ручка*4 + секция (атрибут aSection).
//
// Геометрия и UV повторяют бывшие CPU-эмиттеры (emit_shape_box / add_flora_block_data /
// add_liquid_face_data), включая покачивание верха флоры и волну поверхности воды.
layout(location = 0) in uint aSection;
layout(std430, binding = 2) readonly buffer Quads { uvec4 quads[]; };
layout(std430, binding = 8) readonly buffer Rects { vec4 rects[]; };
layout(std430, binding = 9) readonly buffer Origins { vec4 origins[]; };

// Готовая projection * view (model здесь всегда единичная): одно умножение на вершину вместо трёх.
uniform mat4 view_projection;
uniform float time;
uniform int u_kind;   // 1 shaped, 2 flora, 3 liquid

out vec2 Local;       // shaped: 0..1 по грани бокса
out vec2 TexCoord;    // flora/liquid: готовая UV в атласе
out float NormalY;
out vec3 FragPos;
out float BlockLight;
out float SkyLight;
flat out vec4 Rect;

// Порядок вершин квада по коду угла c = fu + 2*fv. Обычный обход (0,1,3),(0,3,2) — наружу у граней
// -X, -Y, +Z; у +X, +Y, -Z нужен перевёрнутый (0,3,1),(0,2,3) (проверено по векторному произведению).
const int kOrder[6]     = int[6](0, 1, 3, 0, 3, 2);
const int kOrderFlip[6] = int[6](0, 3, 1, 0, 2, 3);

void main() {
    const uint vid = uint(gl_VertexID);
    const uint qi = vid / 6u;
    const uint k = vid - qi * 6u;
    const uint sec = aSection;

    const uvec4 rec = quads[qi];
    const uint w0 = rec.x;
    const vec3 cell = vec3(float(w0 & 31u), float((w0 >> 5u) & 31u), float((w0 >> 10u) & 31u));
    const uint face = (w0 >> 15u) & 7u;
    const uint packed_light = (w0 >> 18u) & 0xFFu;
    const vec4 rect = rects[rec.y & 0x7FFFu];
    const vec3 base = origins[sec >> 2u].xyz + cell;

    vec3 p;
    vec2 tc_param;      // (u, v) в долях спрайта
    float normal_y = 1.0;

    if (u_kind == 2) {
        // Флора: диагональ 6 идёт (x0,z0)->(x1,z1), диагональ 7 — (x1,z0)->(x0,z1).
        const int code = kOrder[k];
        const float fu = float(code & 1);
        const float fv = float((code >> 1) & 1);
        const float px = (face == 7u) ? (1.0 - fu) : fu;
        const float pz = fu;
        p = base + vec3(px, fv, pz);
        if (fv > 0.5) {
            // Верх куста качается на ветру; фаза зависит от мировой клетки угла, как в flora_vertex.glsl.
            const vec2 anchor = floor(p.xz);
            const float phase = anchor.x * 1.7 + anchor.y * 1.3;
            const float sway = sin(time * 1.8 + phase) * 0.09;
            p.x += sway;
            p.z += sway * 0.6;
        }
        tc_param = vec2(fu, fv);
        normal_y = 1.0;
    } else {
        const bool flip = (face == 0u || face == 2u || face == 5u);
        const int code = flip ? kOrderFlip[k] : kOrder[k];
        const float fu = float(code & 1);
        const float fv = float((code >> 1) & 1);
        const bool pos = (face & 1u) == 0u;
        const uint axis = face >> 1u;
        if (u_kind == 1) {
            // Бокс в 1/64 клетки.
            const vec3 lo = vec3(float(rec.z & 127u), float((rec.z >> 7u) & 127u), float((rec.z >> 14u) & 127u)) / 64.0;
            const vec3 hi = vec3(float((rec.z >> 21u) & 127u), float(rec.w & 127u), float((rec.w >> 7u) & 127u)) / 64.0;
            const float plane = pos ? 1.0 : 0.0;
            vec3 r;                                         // положение угла внутри бокса, 0..1 по осям
            if (axis == 0u)      r = vec3(plane, fv, fu);
            else if (axis == 1u) r = vec3(fu, plane, fv);
            else                 r = vec3(fu, fv, plane);
            p = base + mix(lo, hi, r);
            tc_param = vec2(fu, fv);
            normal_y = (axis == 1u) ? (pos ? 1.0 : -1.0) : 0.0;
        } else {
            // Жидкость: высоты углов (fu + 2*fv) лежат в z/w, по 16 бит.
            const uint c = uint(code);
            const uint hw = (c < 2u) ? rec.z : rec.w;
            const float h = float((hw >> ((c & 1u) * 16u)) & 0xFFFFu) / 65535.0;
            const float plane = pos ? 1.0 : 0.0;
            if (axis == 0u) {            // ±X: u вдоль Z, v вдоль Y (верх — высота поверхности)
                p = base + vec3(plane, fv * h, fu);
                tc_param = vec2(fu, fv * h);
            } else if (axis == 1u) {     // ±Y: u вдоль X, v вдоль Z (V перевёрнут, как в push_face_impl)
                p = base + vec3(fu, plane * h, fv);
                tc_param = vec2(fu, 1.0 - fv);
            } else {                     // ±Z
                p = base + vec3(fu, fv * h, plane);
                tc_param = vec2(fu, fv * h);
            }
            normal_y = (axis == 1u) ? (pos ? 1.0 : -1.0) : 0.0;
            // Волна только на верхней грани, боковые и нижние не трогаем (иначе щели у берега).
            if (face == 2u) {
                p.y += sin(p.x * 0.7 + time * 1.3) * 0.035 + cos(p.z * 0.6 + time * 1.7) * 0.035;
            }
        }
    }

    Local = tc_param;
    TexCoord = mix(rect.xy, rect.zw, tc_param);
    NormalY = normal_y;
    FragPos = p;
    SkyLight = float(packed_light >> 4u);
    BlockLight = float(packed_light & 15u);
    Rect = rect;
    gl_Position = view_projection * vec4(p, 1.0);
}
