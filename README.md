# OptiCraft

OptiCraft — учебный C++-проект воксельного игрового движка в духе Minecraft. Проект использует OpenGL, потоковую загрузку чанков, процедурную генерацию мира и GPU-driven рендеринг.

## Текущая версия — 0.10.2

**Добавлено и изменено:** улучшен рендеринг мира: чанки плавно проявляются блоками без деформации границ и щелей на стыках; добавлен лёгкий туман, привязанный к выбранной дальности прорисовки; переработано направленное дневное освещение с сохранением базового уровня сцены 0.32. Новые GPU-меши получают приоритет при отправке на видеокарту, а их compute-построение и подготовка draw-команд остаются в одном GPU-потоке кадра. Очереди задач и готовых мешей учитывают расстояние и реальное направление движения; приоритеты обновляются и внутри чанка игрока. LOD-переходы больше не ждут полного опустошения очередей генерации и мешинга.

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
