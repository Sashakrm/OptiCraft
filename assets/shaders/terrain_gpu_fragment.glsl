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

    // BlockLight и SkyLight (0..15) интерполированы по квадру от углов: свет сглажен, без ступенек.
    // День/ночь влияет ТОЛЬКО на небесный свет, свет блоков в пещерах ночью не тускнеет.
    float sky_scene_light = (SkyLight / 15.0) * ambient;
    float block_scene_light = BlockLight / maxLightLevel;
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

    // Вершинный AO (интерполирован по квадру): закрытые углы темнее, 0 -> 45 %, 3 -> полная яркость.
    float ao_shade = mix(0.45, 1.0, AO / 3.0);
    vec3 lit_color = tex_color.rgb * light_intensity * scene_light * ao_shade;
    float fog_amount = smoothstep(fogRange.x, fogRange.y, distance(WorldPos, cameraPosition));
    FragColor = vec4(mix(lit_color, fogColor, fog_amount), tex_color.a);
}
