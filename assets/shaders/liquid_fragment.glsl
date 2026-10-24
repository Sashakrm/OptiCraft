#version 330 core
out vec4 FragColor;

in vec2 TexCoord;
in vec3 Normal;
in vec3 FragPos;
in float BlockLight;

uniform sampler2D textureAtlas;
uniform float time;
// Левый-нижний угол спрайта воды в UV-координатах атласа и его размер. Спрайт не обязан лежать
// на сетке тайлов (он может быть любого размера), поэтому origin передаётся явно, а не
// вычисляется через floor() — тот давал сдвиг на пиксель у границ спрайта.
uniform vec2 tile_origin;
uniform vec2 tile_size;
uniform float ambient;
uniform float maxLightLevel;

void main() {
    // Работаем в локальных координатах спрайта [0, tile_size): смещение от известного угла
    // спрайта, зацикленное через mod. Никакого floor() и эпсилонов — фрагмент всегда попадает
    // внутрь своего спрайта, поэтому текстура не съезжает.
    vec2 local_uv = TexCoord - tile_origin;

    vec2 flow = vec2(sin(time * 0.15) * 0.15, time * 0.35) * tile_size;
    local_uv = mod(local_uv + flow, tile_size);

    vec4 tex_color = texture(textureAtlas, tile_origin + local_uv);

    float light_intensity = 0.7;
    if (Normal.y > 0.9) {
        light_intensity = 1.0;
    } else if (Normal.y < -0.9) {
        light_intensity = 0.5;
    }

    // Лёгкое мерцание яркости — вместе с волной на вершинах создаёт ощущение живой,
    // не статичной поверхности воды.
    float shimmer = 0.92 + 0.08 * sin(FragPos.x * 1.3 + FragPos.z * 1.1 + time * 2.0);

    float packed_light = floor(BlockLight + 0.5);
    float block_light = mod(packed_light, 16.0);
    float sky_light = floor(packed_light / 16.0);
    float sky_scene_light = (sky_light / 15.0) * ambient;
    float block_scene_light = block_light / maxLightLevel;
    float scene_light = max(max(sky_scene_light, block_scene_light), 0.32);
    FragColor = vec4(tex_color.rgb * light_intensity * shimmer * scene_light, tex_color.a * 0.78);
}
