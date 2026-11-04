#version 330 core
in vec2 vUV;
out vec4 FragColor;

// Пост-проход над готовым кадром мира: сглаживание FXAA (по контрасту яркости соседних пикселей)
// и плёночное зерно в стиле Hollow Knight. Вершинный шейдер — vignette_vertex.glsl (полноэкранный квад).

uniform sampler2D scene;
uniform vec2 texel;       // 1 / размер кадра в пикселях
uniform int useFxaa;      // 1 — включить FXAA
uniform float grain;      // амплитуда зерна в долях яркости (0 — выключено)
uniform float time;       // для анимации зерна: каждый кадр новый рисунок

const vec3 kLuma = vec3(0.299, 0.587, 0.114);

float luma(vec3 c) {
    return dot(c, kLuma);
}

// Упрощённая FXAA: сглаживает только там, где яркость соседей резко меняется. Прямые участки
// и текстуры без контраста остаются нетронутыми, поэтому мелкие детали блоков не размываются.
vec3 apply_fxaa(vec2 uv) {
    vec3 rgbM = texture(scene, uv).rgb;
    float lumaM = luma(rgbM);
    float lumaNW = luma(texture(scene, uv + vec2(-1.0, -1.0) * texel).rgb);
    float lumaNE = luma(texture(scene, uv + vec2( 1.0, -1.0) * texel).rgb);
    float lumaSW = luma(texture(scene, uv + vec2(-1.0,  1.0) * texel).rgb);
    float lumaSE = luma(texture(scene, uv + vec2( 1.0,  1.0) * texel).rgb);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    float lumaRange = lumaMax - lumaMin;
    if (lumaRange < max(0.0312, lumaMax * 0.125)) return rgbM;

    // Направление границы: перпендикулярно градиенту яркости.
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.125), 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * texel;

    vec3 rgbA = 0.5 * (texture(scene, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                       texture(scene, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(scene, uv + dir * -0.5).rgb +
                                     texture(scene, uv + dir * 0.5).rgb);
    float lumaB = luma(rgbB);
    // Если двухшаговая выборка вышла за диапазон соседей, берём более мягкий вариант.
    return (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec3 col = (useFxaa == 1) ? apply_fxaa(vUV) : texture(scene, vUV).rgb;
    if (grain > 0.0) {
        // Новый рисунок зерна каждый кадр; смещение по времени не даёт узору "застыть".
        float n = hash12(gl_FragCoord.xy + vec2(fract(time * 13.7) * 977.0, fract(time * 5.3) * 613.0)) - 0.5;
        col += n * grain;
    }
    FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
