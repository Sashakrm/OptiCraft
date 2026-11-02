#version 430 core
// Фрагментный шейдер блоков формы (u_kind 1), флоры (2) и жидкости (3) GPU-мешера. Освещение — то же,
// что в terrain_gpu_fragment.glsl / flora_fragment.glsl / liquid_fragment.glsl; меняешь одно — меняй все.
out vec4 FragColor;

in vec2 Local;
in vec2 TexCoord;
in float NormalY;
in vec3 FragPos;
in float BlockLight;
in float SkyLight;
flat in vec4 Rect;

uniform sampler2D textureAtlas;
uniform float ambient;
uniform float maxLightLevel;
uniform float time;
uniform int u_kind;
// Левый-нижний угол спрайта воды в UV атласа и его размер (см. liquid_fragment.glsl).
uniform vec2 tile_origin;
uniform vec2 tile_size;

void main() {
    float light_intensity = 0.7;
    if (NormalY > 0.9) light_intensity = 1.0;
    else if (NormalY < -0.9) light_intensity = 0.5;

    vec4 tex_color;
    float shimmer = 1.0;
    float alpha_scale = 1.0;
    if (u_kind == 1) {
        const vec2 f = clamp(fract(Local), 0.0005, 0.9995);
        tex_color = texture(textureAtlas, mix(Rect.xy, Rect.zw, f));
    } else if (u_kind == 2) {
        tex_color = texture(textureAtlas, TexCoord);
        if (tex_color.a < 0.1) discard;
    } else {
        vec2 local_uv = TexCoord - tile_origin;
        const vec2 flow = vec2(sin(time * 0.15) * 0.15, time * 0.35) * tile_size;
        local_uv = mod(local_uv + flow, tile_size);
        tex_color = texture(textureAtlas, tile_origin + local_uv);
        shimmer = 0.92 + 0.08 * sin(FragPos.x * 1.3 + FragPos.z * 1.1 + time * 2.0);
        alpha_scale = 0.78;
    }

    const float sky_scene_light = (SkyLight / 15.0) * ambient;
    const float block_scene_light = BlockLight / maxLightLevel;
    const float scene_light = max(max(sky_scene_light, block_scene_light), 0.32);
    FragColor = vec4(tex_color.rgb * light_intensity * shimmer * scene_light, tex_color.a * alpha_scale);
}
