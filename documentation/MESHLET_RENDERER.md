# Экспериментальный GPU Meshlet Renderer

## Назначение и границы

Meshlet-путь — необязательный режим поверх существующего GPU greedy mesher. Он не заменяет `Chunk_Manager`, генерацию чанков, расчёт освещения или текущий формат геометрии. `mesh_greedy.comp` и `mesh_misc.comp` продолжают заполнять общий пул 16-байтных `uvec4` quad records; новая подсистема добавляет кластерные AABB, более мелкий culling и отдельный массив MDI-команд.

Это GPU-driven аналог некоторых принципов Task/Mesh Shading, но не аппаратный Mesh Shader. Геометрия по-прежнему доходит до обычного vertex shader через SSBO/Vertex Pulling, затем проходит штатную растеризацию OpenGL. В текущей интеграции CPU формирует список stream-кандидатов на кадр и вызывает обычный `glDispatchCompute`; дочерние Compute Workgroups не запускаются аппаратно из другого shader stage.

## Переключение

Внутри мира открой **Настройки → Путь рендера**:

- `Стандартный` — прежний renderer, включая исходный выбор классического vertex pulling или доступного EXT/NV mesh-shader backend.
- `GPU Meshlet (эксперимент.)` — отдельные Meshlet AABB и GPU MDI для непрозрачных кубов, блоков формы и воды.

Настройка сохраняется в `options.txt` как `meshlet_renderer=0|1`. Режим выключен по умолчанию. Если шейдеры/буферы Meshlet не удалось создать, доступность переключателя снимается и используется стандартный renderer.

## Какие типы геометрии переводятся

| Вид | Поведение в Meshlet-режиме |
|---|---|
| Solid greedy quads | Meshlet AABB + per-meshlet culling + MDI + `terrain_gpu_vertex.glsl` |
| Shaped quads | Meshlet AABB из box bounds + per-meshlet culling + MDI + `terrain_gpu_misc_vertex.glsl` с `u_kind=1` |
| Flora | Существующий chunk-culling/MDI путь |
| Liquid | Отдельный Meshlet stream (kind 3), MDI range и count; far-to-near порядок, frustum-only culling |

Meshlet-режим использует классические vertex-pulling программы для всего кадра, чтобы команды opaque и transparent потоков были совместимы в одном renderer. Поэтому при включённом Meshlet-пути нативный `GL_EXT_mesh_shader`/`GL_NV_mesh_shader` draw не вызывается. При выключенном режиме нативный путь не меняется.

## Данные и ABI

### Quad pool и Meshlet descriptors

Allocator существующего quad pool выдаёт диапазоны, выровненные по `kGpuBlockQuads = 32`. Descriptor индексируется как `firstQuad / 32`; его `std430` layout:

```glsl
struct MeshletBounds {
    vec4 minimum;   // world-space AABB min
    vec4 maximum;   // world-space AABB max
    uvec4 metadata; // x = число реальных quad records (0..32)
};
```

Размер — 48 байт, соответствует `Gpu_Meshlet_Bounds` в C++. Descriptor buffer растёт вместе с quad pool; прежние entries копируются. Освобождённые диапазоны могут переиспользовать тот же слот, поэтому новый geometry upload обязан обновить descriptor до его отрисовки.

### Solid layout

Для greedy solid quad builder читает `rec.x` так же, как `terrain_gpu_vertex.glsl`:

- bits 0..4 / 5..9 / 10..14 — cell X/Y/Z;
- bits 15..16 — axis;
- bit 17 — positive face;
- bits 18..22 — width минус 1;
- bits 23..27 — height минус 1;
- bit 28 — UV rotation;
- bits 29..30 — normal extent exponent;
- bit 31 — baked-light flag.

Плоская грань получает консервативную толщину ±0.002 world unit по normal axis, чтобы float precision не породила ложный cull.

### Shaped layout

`mesh_misc.comp::make_rec` пишет другой формат. Для shaped face:

- `rec.x` содержит cell X/Y/Z, face и packed light;
- `rec.z` содержит `lo.x`, `lo.y`, `lo.z`, `hi.x` по 7 бит;
- `rec.w` содержит `hi.y`, `hi.z` по 7 бит;
- координаты бокса заданы в 1/64 клетки.

Builder распознаёт kind по индексу stream (`(stream % 16) / 4`). Для shaped-геометрии использует именно box bounds, для liquid — полную исходную клетку с вертикальным запасом 0.08 блока под анимацию верхней грани. Группировка нескольких граней/боксов в одном 32-quad Meshlet выполняется через union AABB. Flora descriptors не используются Meshlet-путём и помечаются пустыми.

## GPU passes и bindings

### 1. `gpu_build_meshlets.comp`

Выполняется только после обновления quad geometry либо при первом включении Meshlet-режима, если геометрия была построена ранее.

| Binding | Buffer | Содержимое |
|---:|---|---|
| 0 | BuildCandidates | `uvec4(handle, streamIndex, firstQuad, streamOffset)` |
| 1 | Counts | Существующие quad counts по handle/kind/section |
| 2 | Quads | Существующий 16-byte quad pool |
| 3 | Origins | Мировые origins по handle |
| 4 | MeshletBounds | Persistent 48-byte descriptors |

Каждый invocation строит один descriptor (до 32 quad records). После dispatch вызывается `GL_SHADER_STORAGE_BARRIER_BIT`, чтобы следующий compute culler видел новые bounds. Для больших списков candidate CPU разбивает dispatch по `GL_MAX_COMPUTE_WORK_GROUP_COUNT.x` и передаёт `u_candidate_base`.

### 2. `gpu_cull_meshlets.comp`

CPU на кадр формирует компактный список stream records только для resident chunk sections, которые имеют solid, shaped или liquid geometry. Opaque solid/shaped streams идут near-to-far; жидкие streams добавляются в обратном порядке chunk list, чтобы сохранить прозрачную сортировку. Один workgroup соответствует одному (handle, kind, section) stream. 32 invocation обрабатывают 32 Meshlet за batch и повторяют проход при большем количестве.

| Binding | Buffer | Содержимое |
|---:|---|---|
| 0 | Streams | пары `uvec4 range + uvec4 metadata` |
| 1 | MeshletBounds | persistent descriptor pool |
| 2 | Commands | `DrawArraysIndirectCommand`, 16 байт (`uvec4`) |
| 3 | DrawCounts | счётчики solid и shaped для indirect-count API |

Поля stream record имеют фиксированный ABI:

- `range = {handle, globalStreamIndex, firstQuad, flags}`; bit 0 marks a coarse LOD stream;
- `metadata = {section, kind, fixedCommandOffset, meshletCount}`.

Bit 0 in `range.w` marks a coarse LOD stream. Such streams keep frustum culling but skip
coarse Hi-Z rejection: AABB projection of wide/flat coarse faces can overlap unrelated depth
samples, and a false rejection appears as isolated LOD pinholes. The final full-resolution
`GL_LESS` depth test still hides pixels covered by nearer opaque surfaces. Legacy asynchronous
per-section occlusion-query results are not consumed by the gameplay Meshlet culler.

Проверки выполняются в порядке: пустой descriptor → frustum → для детальных opaque streams — conservative screen-space occlusion по текущему кадру Hi-Z; LOD и liquid не используют Hi-Z. Box, пересекающий eye/near plane, остаётся видимым. Окно screen-space расширяется guard pixels. Для больших списков streams dispatch разбивается по `GL_MAX_COMPUTE_WORK_GROUP_COUNT.x` через `u_stream_base`.

### 3. Команды отрисовки

Команда имеет layout OpenGL `DrawArraysIndirectCommand`:

```cpp
struct DrawArraysIndirectCommand {
    uint32_t count;         // quadCount * 6
    uint32_t instanceCount; // 1
    uint32_t first;         // firstQuad * 6
    uint32_t baseInstance;  // handle * Config::chunk_section_count + section
};
```

Индексы solid, shaped и liquid лежат в трёх непрерывных диапазонах одного командного буфера. В fixed-count fallback culler пишет команду для каждого possible Meshlet, а у невидимых выставляет `count=0`; draw вызывается через `glMultiDrawArraysIndirect`. При наличии runtime entry point `glMultiDrawArraysIndirectCount` culler compact-ит только видимые команды и записывает фактическое количество в `DrawCounts`; draw читает этот счётчик через parameter buffer binding. Наличие core/ARB функции проверяется во время инициализации, а не предполагается по одной лишь версии OpenGL.

В compact mode каждый workgroup использует `shared` counter и локальный `atomicAdd`; один global `atomicAdd` резервирует диапазон на batch из 32 Meshlet для своего kind. `command_capacity` заранее суммируется по максимальным stream capacities и является верхней границей для output и indirect `maxdrawcount`.

## Синхронизация

- После GPU greedy/misc meshing перед чтением quads/counts в builder используется shader-storage barrier.
- После `gpu_build_meshlets.comp` — `GL_SHADER_STORAGE_BARRIER_BIT` перед чтением AABB в culler.
- После `gpu_cull_meshlets.comp` — `GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT`; при compact indirect-count — дополнительно `GL_PARAMETER_BUFFER_BARRIER_BIT`.
- При загрузке stream/command/count buffers используется orphaning через `glBufferData`, чтобы не перезаписывать storage предыдущего кадра, ещё занятого GPU.
- Подготовка Meshlet-команд выполняется до legacy chunk culler. Если allocation или проверка capacity не проходит, Meshlet mode отключается до того, как старый culler запишет команды: стандартная opaque-геометрия остаётся видимой уже в текущем кадре.

## Ограничения и ожидаемые расходы

- CPU всё ещё обходит видимые chunks/sections и загружает stream descriptors каждый кадр. Это не полностью autonomous GPU scheduler.
- Число Workgroups сейчас равно числу stream records (батчи по аппаратному максимуму); отдельный GPU-generated `glDispatchComputeIndirect` этап здесь сознательно не добавлен: число streams известно после CPU candidate build, а дополнительный dispatch-builder создал бы ещё один проход.
- Culling granularity — 32 quads; это начальная настройка, а не гарантированно оптимальный размер для Intel UHD. Слишком мелкий Meshlet увеличит число AABB-тестов и команд, слишком крупный ослабит occlusion culling.
- Gameplay culler строит Hi-Z каждый кадр из отдельного half-resolution opaque depth target. Это depth prepass для GPU visibility; полноразмерный colour pass всё равно пишет свой depth через `GL_LESS`.
- F3 chunk/section/triangle counters остаются приблизительными chunk-level статистиками старого culler; они не равны числу фактически выданных Meshlet-команд.
- GPU command append / MDI не гарантирует прироста FPS. Перестройка bounds, CPU stream upload, memory pressure и лишние workgroups могут оказаться дороже выигрыша от более мелкого culling.
- Вода переведена в Meshlet MDI, но остаётся обычной прозрачной геометрией: chunk streams идут far-to-near, Hi-Z/opaque query rejection для неё отключены, blending остаётся alpha blend, depth writes выключены. Flora остаётся на прежнем пути.

## Проверка и A/B protocol

В среде разработки новые Compute shaders были скомпилированы и слинкованы в surfaceless Mesa OpenGL 4.5 context. Те же проверки доступны в `tools/meshlet_validation/` и запускаются из корня проекта командами `python3 tools/meshlet_validation/check_glsl_link_egl.py` и `python3 tools/meshlet_validation/test_meshlet_compute.py`. Synthetic GPU dispatch дополнительно проверил:

- solid descriptor: AABB `(0,0,0.998)..(1,1,1.002)`;
- shaped descriptor: AABB `(1,2,3)..(1.5,2.5,3.5)` из 1/64 box data;
- liquid descriptor: bounds исходной клетки `(0,-0.08,0)..(1,1.08,1)`;
- frustum output: отдельные ожидаемые MDI-команды для solid, shaped и liquid;
- compact output: счётчики `[1,1,0,1]` по GPU kind и три команды;
- Hi-Z policy: LOD stream с `range.w bit 0` остаётся видимым, обычный opaque stream behind искусственной near-depth маски отклоняется, liquid остаётся видимым.

Полная C++ сборка игры в данной среде не подтверждена: CMake останавливается на `FindOpenGL`, так как отсутствуют OpenGL development headers/libraries. Остальные зависимости не проверены после этой ошибки. Сам проект не запускался на Intel UHD, поэтому корректность визуального результата на игровом кадре и FPS-профиль нужно подтвердить на машине пользователя.

Для сравнения Standard и GPU Meshlet использовать:

1. Один и тот же мир/seed, камеру, разрешение, render distance, LOD, погоду и графические настройки.
2. Прогреть мир до завершения генерации и начального remesh; затем записать Standard и Meshlet несколько раз, меняя порядок режимов.
3. Снимать GPU frame time и CPU frame time отдельно; сравнивать p50/p95/p99, а не только средний FPS.
4. Повторить тесты: неподвижная открытая местность, плотный лес, горы/много заслонений, пещера, быстрый полёт с chunk streaming, shaped blocks, вода/флора.
5. Отдельно фиксировать render distance 12 и 20 (или выбранные пользователем значения), число visible chunks/sections и число draw calls.
6. Не считать однократное включение режима честным A/B: первый кадр может перестроить persistent AABB descriptors; после него нужен прогрев.
