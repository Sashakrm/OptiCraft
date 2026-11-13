#version 430 core
// Фрагментный шейдер предпрохода глубины (см. terrain_gpu_depth_vertex.glsl). Цвета нет: остаётся только
// тот же discard появления чанка, что в terrain_gpu_fragment.glsl, чтобы глубина совпадала с цветовым
// проходом и растворяющиеся блоки не становились ложными окклюдерами.
in vec3 WorldPos;
flat in vec3 WorldNormal;
flat in float Appearance;

float appearance_hash(vec3 cell_position) {
    vec3 p = fract(cell_position * vec3(0.1031, 0.11369, 0.13787));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

void main() {
    const vec3 block_cell = floor(WorldPos - WorldNormal * 0.001);
    if (appearance_hash(block_cell) > Appearance) discard;
}
