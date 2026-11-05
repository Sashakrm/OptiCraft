#pragma once

// Обёртка над Tracy-профайлером (https://github.com/wolfpld/tracy).
//
//   OC_ZONE("имя")          — замер области от этой строки до конца текущего блока {}.
//   OC_FRAME_MARK            — отметка конца кадра (для счётчика FPS в Tracy).
//   OC_PLOT("имя", value)    — числовой график (например, число грязных чанков).
//
// Если папка external/tracy отсутствует или проект собран с -DOPTICRAFT_TRACY=OFF,
// все макросы превращаются в пустые выражения и не стоят ничего.
// Сам клиент включается определением TRACY_ENABLE (см. CMakeLists.txt).

#if __has_include("tracy/Tracy.hpp")
#include "tracy/Tracy.hpp"
#define OC_ZONE(name) ZoneScopedN(name)
#define OC_FRAME_MARK FrameMark
#define OC_PLOT(name, value) TracyPlot(name, static_cast<double>(value))
#else
#define OC_ZONE(name) ((void)0)
#define OC_FRAME_MARK ((void)0)
#define OC_PLOT(name, value) ((void)0)
#endif
