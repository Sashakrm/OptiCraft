#version 430 core
// Фрагментный шейдер GPU-мешера. Это terrain_fragment.glsl в версии 430: вершинный выход GPU-пути
// (terrain_gpu_vertex.glsl) и CPU-путь (terrain_vertex.glsl) делят один и тот же набор varying.
// Если меняешь освещение или UV здесь, меняй и в terrain_fragment.glsl.
out vec4 FragColor;

in vec2 Local;
in float NormalY;
in vec3 WorldPos;
in vec3 WorldNormal;
in float BlockLight;
in float SkyLight;
in float Appearance;
flat in vec4 Rect;
in float AO;
// Baked = 1: SkyLight/BlockLight/AO — запечённые в квад значения (дальний LOD, откат).
// Baked = 0: свет и AO считаются на пикселе из светового объёма ручки VolumeHandle (ближние чанки).
flat in uint Baked;
flat in uint VolumeHandle;
flat in vec3 ChunkOrigin;

// Световой объём ручек (mesh_volume.comp): 34 x 34 x 34 байта на ручку подряд; 0xFF — непрозрачная клетка.
layout(std430, binding = 10) readonly buffer LightVolumes { uint volume_words[]; };

uniform sampler2D textureAtlas;
// Множитель дня/ночи (см. Day_Night_Cycle::get_ambient_intensity) — 1.0 днём, тусклее ночью.
uniform float ambient;
// Максимальный уровень света от блока (Config::max_light_level), для нормализации BlockLight.
uniform float maxLightLevel;
uniform vec3 cameraPosition;
uniform vec3 fogColor;
uniform vec2 fogRange;
uniform vec3 sunDirection;

float appearance_hash(vec3 cell_position) {
    vec3 p = fract(cell_position * vec3(0.1031, 0.11369, 0.13787));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

// Размер объёма ручки в словах: 34 * 34 * 34 / 4. Порядок байтов — как в mesh_volume.comp.
const uint kVolumeWords = 9826u;

uint volume_byte(ivec3 c) {
    const uint idx = uint((c.y + 1) * 1156 + (c.z + 1) * 34 + (c.x + 1));
    return (volume_words[VolumeHandle * kVolumeWords + (idx >> 2u)] >> ((idx & 3u) * 8u)) & 0xFFu;
}

int volume_outside_axes(ivec3 c) {
    return int(c.x < 0 || c.x >= 32) + int(c.y < 0 || c.y >= 32) + int(c.z < 0 || c.z >= 32);
}

// Свет и AO пикселя грани с объёма ручки. Считаются четыре угла клетки грани в плоскости слоя перед ней
// (та же формула, что face_ao и face_corner_light в mesh_greedy.comp: AO по трём соседним клеткам слоя,
// свет — среднее по четырём клеткам вокруг угла без непрозрачных и кольца), затем билинейно по пикселю.
// Итог — значения на углах клетки, а не на углах объединённого квада, поэтому слитый квад даёт
// те же значения, что и мелкие квады.
void volume_face_light(out float sky, out float blk, out float ao) {
    const vec3 lp = WorldPos - ChunkOrigin;
    const int axis = abs(WorldNormal.x) > 0.5 ? 0 : (abs(WorldNormal.y) > 0.5 ? 1 : 2);
    const int sgn = WorldNormal[axis] > 0.0 ? 1 : -1;
    // Твёрдая клетка грани и клетка перед ней (слой, из которого берётся свет и AO).
    const ivec3 cell = ivec3(floor(lp - WorldNormal * 0.001));
    ivec3 front = cell;
    front[axis] += sgn;
    const int ua = axis == 0 ? 2 : 0;
    const int va = axis == 1 ? 2 : 1;
    const float fu = clamp(lp[ua] - float(cell[ua]), 0.0, 1.0);
    const float fv = clamp(lp[va] - float(cell[va]), 0.0, 1.0);

    float c_sky[4];
    float c_blk[4];
    float c_ao[4];
    for (int cv = 0; cv < 2; ++cv) {
        for (int cu = 0; cu < 2; ++cu) {
            const int du = cu == 1 ? 1 : -1;
            const int dv = cv == 1 ? 1 : -1;
            const ivec3 p0 = front;
            ivec3 p1 = front; p1[ua] += du;
            ivec3 p2 = front; p2[va] += dv;
            ivec3 p3 = front; p3[ua] += du; p3[va] += dv;
            const uint b1 = volume_byte(p1);
            const uint b2 = volume_byte(p2);
            const uint b3 = volume_byte(p3);
            const bool s1 = b1 == 0xFFu;
            const bool s2 = b2 == 0xFFu;
            const bool s3 = b3 == 0xFFu;
            const int level = (s1 && s2) ? 0 : 3 - (int(s1) + int(s2) + int(s3));

            // Свет: четыре клетки вокруг угла. Клетки за двумя границами чанка и непрозрачные не учитываются.
            const ivec3 pts[4] = ivec3[4](p0, p1, p2, p3);
            const uint cell_bytes[4] = uint[4](volume_byte(p0), b1, b2, b3);
            int sky_sum = 0;
            int blk_sum = 0;
            int count = 0;
            for (int i = 0; i < 4; ++i) {
                if (volume_outside_axes(pts[i]) >= 2 || cell_bytes[i] == 0xFFu) continue;
                sky_sum += int(cell_bytes[i] >> 4u);
                blk_sum += int(cell_bytes[i] & 15u);
                ++count;
            }
            count = max(count, 1);
            const int ci = cu + 2 * cv;
            c_sky[ci] = float((sky_sum + count / 2) / count);
            c_blk[ci] = float((blk_sum + count / 2) / count);
            c_ao[ci] = float(level);
        }
    }
    sky = mix(mix(c_sky[0], c_sky[1], fu), mix(c_sky[2], c_sky[3], fu), fv);
    blk = mix(mix(c_blk[0], c_blk[1], fu), mix(c_blk[2], c_blk[3], fu), fv);
    ao = mix(mix(c_ao[0], c_ao[1], fu), mix(c_ao[2], c_ao[3], fu), fv);
}

void main() {
    // Each voxel reveals as one stable unit. The face-normal offset maps all six
    // faces back to the same block cell, avoiding different dissolve patterns per face.
    const vec3 block_cell = floor(WorldPos - WorldNormal * 0.001);
    if (appearance_hash(block_cell) > Appearance) discard;

    float light_intensity = 0.7;
    if (NormalY > 0.9) light_intensity = 1.0;
    else if (NormalY < -0.9) light_intensity = 0.5;

    // Текстура блока повторяется внутри слитого квада: fract даёт позицию внутри клетки.
    // Зажим на долю пикселя не даёт сэмплеру заходить на соседний спрайт атласа у шва клетки.
    vec2 f = clamp(fract(Local), 0.0005, 0.9995);
    vec2 uv = mix(Rect.xy, Rect.zw, f);
    vec4 tex_color = texture(textureAtlas, uv);

    // Свет (0..15) сглажен: для ближних чанков считается на пикселе из объёма, для запечённых — интерполирован
    // по квадру от углов. День/ночь влияет ТОЛЬКО на небесный свет, свет блоков в пещерах ночью не тускнеет.
    float sky_light = SkyLight;
    float block_light = BlockLight;
    float ao_level = AO;
    if (Baked == 0u) volume_face_light(sky_light, block_light, ao_level);
    float sky_scene_light = (sky_light / 15.0) * ambient;
    float block_scene_light = block_light / maxLightLevel;
    // Не допускаем абсолютно чёрных поверхностей во время потокового обновления света.
    // Мягкий направленный свет от солнца добавляет объём боковым граням. Нижняя
    // граница 0.32 сохранена намеренно: это выбранный базовый уровень сцены.
    const float sun_term = max(dot(normalize(WorldNormal), normalize(sunDirection)), 0.0);
    // Directional sunlight is strongest above the horizon and fades smoothly at dawn/dusk.
    // At night the existing ambient/block-light model remains unchanged. The 0.32 scene floor
    // is intentional and must stay stable while this directional contrast is added.
    const float sun_strength = smoothstep(-0.12, 0.10, sunDirection.y);
    const float direct_light = mix(0.80, 1.08, sun_term);
    const float directional_light = mix(1.0, direct_light, sun_strength);
    const float scene_light = max(max(sky_scene_light, block_scene_light) * directional_light, 0.32);

    // AO: закрытые углы темнее, 0 -> 45 %, 3 -> полная яркость.
    float ao_shade = mix(0.45, 1.0, ao_level / 3.0);
    vec3 lit_color = tex_color.rgb * light_intensity * scene_light * ao_shade;
    float fog_amount = smoothstep(fogRange.x, fogRange.y, distance(WorldPos, cameraPosition));
    FragColor = vec4(mix(lit_color, fogColor, fog_amount), tex_color.a);
}
