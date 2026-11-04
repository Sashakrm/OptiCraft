# OptiCraft

OptiCraft — учебный C++-проект воксельного игрового движка в духе Minecraft. Проект использует OpenGL, потоковую загрузку чанков, процедурную генерацию мира и GPU-driven рендеринг.

## Текущая версия — 0.10.3

**Добавлено и изменено:** в меню «Настройки» появились параметры графики: длительность появления чанка (0.10–0.50 с, по умолчанию 0.42 с); дальность прорисовки по высоте (ползунок «Авто» или 2–12 чанков — «Авто» считает значение от горизонтальной дальности, например 20 чанков → 7); сглаживание (выкл / FXAA); зерно (ползунок, в стиле Hollow Knight); виньетка (выкл, слабая, средняя, сильная). Значения сохраняются в `options.txt`; по умолчанию вид игры не меняется.

## Сборка

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug -j 4
```

## Документация

- [`documentation/history_version.md`](documentation/history_version.md) — история версий и изменений.
- [`documentation/TECHNICAL_DOCUMENTATION.md`](documentation/TECHNICAL_DOCUMENTATION.md) — общая техническая документация проекта.
- [`documentation/RENDERING_PIPELINE.md`](documentation/RENDERING_PIPELINE.md) — устройство конвейера рендеринга.
- [`documentation/WORLDGEN_PERFORMANCE.md`](documentation/WORLDGEN_PERFORMANCE.md) — генерация мира и её производительность.
- [`documentation/KNIGHT_AI.md`](documentation/KNIGHT_AI.md) — документация ИИ босса-рыцаря.
