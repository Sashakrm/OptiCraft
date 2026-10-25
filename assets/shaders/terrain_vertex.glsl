#version 330 core
// Непрозрачная геометрия чанков после жадного меширования (см. Terrain_Vertex в Chunk.h).
// Один квад может покрывать прямоугольник из многих одинаковых граней, поэтому UV приходят как
// локальные координаты в блоках, а прямоугольник спрайта в атласе — отдельным атрибутом.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aLocal;
layout (location = 2) in float aNormalY;
layout (location = 3) in float aLight;
layout (location = 4) in vec4 aRect;   // u_min, v_min, u_max, v_max спрайта в атласе
layout (location = 5) in float aAO;    // вершинный AO, 0..3
layout (location = 6) in float aSky;  // небесный свет вершины, 0..15

out vec2 Local;
out float NormalY;
out float BlockLight;
flat out vec4 Rect;
out float AO;
out float SkyLight;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main() {
    Local = aLocal;
    NormalY = aNormalY;
    BlockLight = aLight;
    SkyLight = aSky;
    Rect = aRect;
    AO = aAO;
    gl_Position = projection * view * model * vec4(aPos, 1.0);
}
