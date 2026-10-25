#version 330 core
out vec4 FragColor;

in vec2 Local;
in float NormalY;
in float BlockLight;
in float SkyLight;
flat in vec4 Rect;
in float AO;

uniform sampler2D textureAtlas;
// Множитель дня/ночи (см. Day_Night_Cycle::get_ambient_intensity) — 1.0 днём, тусклее ночью.
uniform float ambient;
// Максимальный уровень света от блока (Config::max_light_level), для нормализации BlockLight.
uniform float maxLightLevel;

void main() {
    float light_intensity = 0.7;
    if (NormalY > 0.9) light_intensity = 1.0;        // верхняя грань — ярче
    else if (NormalY < -0.9) light_intensity = 0.5;  // нижняя — темнее

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
    float scene_light = max(max(sky_scene_light, block_scene_light), 0.32);

    // Вершинный AO (интерполирован по квадру): закрытые углы темнее, 0 -> 45 %, 3 -> полная яркость.
    float ao_shade = mix(0.45, 1.0, AO / 3.0);
    FragColor = vec4(tex_color.rgb * light_intensity * scene_light * ao_shade, tex_color.a);
}
