cmake_minimum_required(VERSION 3.16)
project(OptiCraft)

# =============== Настройки ===============
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Без этого блока `cmake ..` без явного -DCMAKE_BUILD_TYPE собирает с -O0, то есть
# вообще без оптимизаций. Для генератора мира, который состоит из вызовов шума,
# это само по себе замедление в несколько раз.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Тип сборки" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
            Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "CMAKE_BUILD_TYPE не задан — используется Release")
endif()

# Нативные инструкции ускоряют шум, но собранный бинарник перестаёт запускаться
# на более старых процессорах. По умолчанию выключено; включается через
# -DOPTICRAFT_NATIVE_ARCH=ON, когда сборка только для своей машины.
option(OPTICRAFT_NATIVE_ARCH "Оптимизировать под процессор сборочной машины" OFF)

# Межмодульная оптимизация (LTO): заметнее всего ускоряет мелкие функции шума/сеток между .cpp.
# Выключена по умолчанию — линковка заметно дольше. Включается -DOPTICRAFT_LTO=ON.
option(OPTICRAFT_LTO "Включить LTO в Release" OFF)

# Удобно для IDE (VS Code, CLion и т.д.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============== Пути ===============
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# =============== Все исходные файлы ===============
set(SOURCES
        main.cpp

        # commands
        ${SRC_DIR}/commands/Command_System.cpp

        # core
        ${SRC_DIR}/core/app/Application.cpp
        ${SRC_DIR}/core/game/Game.cpp
        ${SRC_DIR}/core/game/Day_Night_Cycle.cpp
        ${SRC_DIR}/core/game/Tick.cpp
        ${SRC_DIR}/mobs/Mobs_Update.cpp

        # entities
        ${SRC_DIR}/entities/player/Camera.cpp
        ${SRC_DIR}/entities/player/Player.cpp
        ${SRC_DIR}/entities/player/Player_Model.cpp
        ${SRC_DIR}/entities/player/Inventory.cpp
        ${SRC_DIR}/entities/items/Dropped_Item.cpp

        # input
        ${SRC_DIR}/input/Input_Manager.cpp

        # rendering
        ${SRC_DIR}/rendering/capture/Isometric_Capture.cpp
        ${SRC_DIR}/rendering/Renderer.cpp
        ${SRC_DIR}/rendering/Shader.cpp
        ${SRC_DIR}/rendering/atlas/Texture_Atlas.cpp
        ${SRC_DIR}/rendering/atlas/Atlas_Registry.cpp

        # utils
        ${SRC_DIR}/utils/logging/Logger.cpp
        ${SRC_DIR}/utils/text/Json.cpp
        ${SRC_DIR}/utils/binary/Binary_Json.cpp
        ${SRC_DIR}/utils/text/Localization.cpp

        # world
        ${SRC_DIR}/world/blocks/Block.cpp
        ${SRC_DIR}/world/blocks/Block_Entity.cpp
        ${SRC_DIR}/world/blocks/Block_Entity_Store.cpp
        ${SRC_DIR}/world/crafting/Recipe.cpp
        ${SRC_DIR}/world/crafting/Crop_Manager.cpp
        ${SRC_DIR}/world/crafting/Sapling_Manager.cpp
        ${SRC_DIR}/world/logic/Logic_Manager.cpp
        ${SRC_DIR}/world/crafting/Smelting.cpp
        ${SRC_DIR}/world/storage/Region_File.cpp
        ${SRC_DIR}/world/storage/Chunk_IO.cpp
        ${SRC_DIR}/world/generation/Biome.cpp
        ${SRC_DIR}/world/generation/Cave_Network.cpp
        ${SRC_DIR}/world/generation/River_Network.cpp
        ${SRC_DIR}/world/generation/Structure_Placement.cpp
        ${SRC_DIR}/world/blocks/Block_Interaction.cpp
        ${SRC_DIR}/world/chunks/Chunk.cpp
        ${SRC_DIR}/world/chunks/Chunk_Manager.cpp
        ${SRC_DIR}/world/generation/Generation_Type.cpp
        ${SRC_DIR}/world/generation/Structures.cpp
        ${SRC_DIR}/world/generation/Tree_Generator.cpp
        ${SRC_DIR}/world/generation/World_Generator.cpp
        ${SRC_DIR}/world/storage/World_File.cpp
        ${SRC_DIR}/world/fluids/Fluid_Simulator.cpp
        ${SRC_DIR}/world/dimensions/Dimension.cpp

        # physics
        ${SRC_DIR}/physics/Raycast.cpp
        ${SRC_DIR}/physics/Liquid_Physics.cpp

        # hud
        ${SRC_DIR}/hud/Hud.cpp
        ${SRC_DIR}/hud/Hud_Screens.cpp
        ${SRC_DIR}/hud/layouts/Panel_Layouts.cpp

        # mobs
        ${SRC_DIR}/mobs/Mob_Model.cpp
        ${SRC_DIR}/mobs/Mob.cpp

        # entities/AI
        ${SRC_DIR}/entities/animals/ai/Pig_AI.cpp
        ${SRC_DIR}/entities/animals/Pig.cpp
        ${SRC_DIR}/entities/animals/Passive_Animal.cpp
        ${SRC_DIR}/entities/hostile/Hostile_Mob.cpp
        ${SRC_DIR}/entities/animals/ai/Horse_AI.cpp
        ${SRC_DIR}/entities/animals/Horse.cpp
        src/world/blocks/Block_Shapes.cpp
        src/entities/effects/Status_Effects.cpp
)

set(PLATFORM_SOURCES
        ${SRC_DIR}/core/app/Window.cpp
        ${SRC_DIR}/world/blocks/Block_Types.cpp
        ${SRC_DIR}/rendering/atlas/Texture.cpp
)

set(THIRD_PARTY_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/external/glad/src/gl.c
        # шум для генерации мира (OpenSimplex2S, C)
        ${CMAKE_CURRENT_SOURCE_DIR}/external/opensimplex/OpenSimplex2S.c
)

# =============== Инструмент json -> OCJB (tools/json2bin) ===============
# Переводит ассеты (blocks.json, рецепты, атласы, ...) в бинарный формат *.ocb и читает их обратно.
add_executable(json2bin
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/json2bin/json2bin.cpp
        ${SRC_DIR}/utils/binary/Binary_Json.cpp
        ${SRC_DIR}/utils/text/Json.cpp
        ${SRC_DIR}/utils/logging/Logger.cpp
)
target_include_directories(json2bin PRIVATE ${SRC_DIR})

# =============== Создаём исполняемый файл ===============
add_executable(${PROJECT_NAME}
        ${SOURCES}
        ${PLATFORM_SOURCES}
        ${THIRD_PARTY_SOURCES}
)

# Добавляем заголовочные файлы (чтобы они отображались в IDE)
target_sources(${PROJECT_NAME} PRIVATE
        ${SRC_DIR}/commands/Command_System.h
        ${SRC_DIR}/core/app/Application.h
        ${SRC_DIR}/core/game/Game.h
        ${SRC_DIR}/core/game/Tick.h
        ${SRC_DIR}/core/app/Window.h

        ${SRC_DIR}/entities/player/Camera.h
        ${SRC_DIR}/entities/player/Player.h
        ${SRC_DIR}/entities/items/Dropped_Item.h

        ${SRC_DIR}/input/Input_Call_Backs.h
        ${SRC_DIR}/input/Input_Manager.h

        ${SRC_DIR}/rendering/Frustum.h
        ${SRC_DIR}/rendering/capture/Isometric_Capture.h
        ${SRC_DIR}/rendering/Renderer.h
        ${SRC_DIR}/rendering/Shader.h
        ${SRC_DIR}/rendering/atlas/Texture_Atlas.h
        ${SRC_DIR}/rendering/atlas/Atlas_Registry.h

        ${SRC_DIR}/utils/Config.h
        ${SRC_DIR}/math/Hash_Utils.h
        ${SRC_DIR}/utils/logging/Logger.h
        ${SRC_DIR}/math/Math_Helpers.h
        ${SRC_DIR}/math/PerlinNoise.h
        ${SRC_DIR}/rendering/atlas/Texture.h
        ${SRC_DIR}/utils/text/Json.h
        ${SRC_DIR}/utils/text/Localization.h

        ${SRC_DIR}/world/blocks/Block.h
        ${SRC_DIR}/world/blocks/Block_Entity.h
        ${SRC_DIR}/world/blocks/Block_Entity_Store.h
        ${SRC_DIR}/world/crafting/Recipe.h
        ${SRC_DIR}/world/crafting/Smelting.h
        ${SRC_DIR}/world/blocks/Container_Session.h
        ${SRC_DIR}/world/blocks/Block_Types.h
        ${SRC_DIR}/world/generation/Biome.h
        ${SRC_DIR}/world/generation/Cave_Network.h
        ${SRC_DIR}/world/generation/River_Network.h
        ${SRC_DIR}/world/generation/Structure_Placement.h
        ${SRC_DIR}/world/blocks/Block_Interaction.h
        ${SRC_DIR}/world/chunks/Chunk.h
        ${SRC_DIR}/world/chunks/Chunk_Manager.h
        ${SRC_DIR}/world/generation/Generation_Type.h
        ${SRC_DIR}/world/generation/Structures.h
        ${SRC_DIR}/world/generation/Tree_Generator.h
        ${SRC_DIR}/world/generation/World_Generator.h
        ${SRC_DIR}/world/storage/World_File.h
        ${SRC_DIR}/world/storage/Region_File.h
        ${SRC_DIR}/world/storage/Chunk_IO.h

        ${SRC_DIR}/physics/Raycast.h

        ${SRC_DIR}/hud/Hud.h
        ${SRC_DIR}/hud/layouts/Panel_Layouts.h

        ${SRC_DIR}/mobs/Mob_Model.h
        ${SRC_DIR}/mobs/Mob.h
        ${SRC_DIR}/mobs/Mobs_Update.h

        ${SRC_DIR}/entities/animals/ai/Pig_AI.h
        ${SRC_DIR}/entities/animals/Pig.h
        ${SRC_DIR}/entities/animals/ai/Horse_AI.h
        ${SRC_DIR}/entities/animals/Horse.h
        ${SRC_DIR}/entities/animals/Passive_Animal.h
        ${SRC_DIR}/entities/hostile/Hostile_Mob.h
)

# =============== Флаги оптимизации ===============
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/O2 /fp:fast>)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:RelWithDebInfo>:-O2>)
    if(OPTICRAFT_NATIVE_ARCH)
        target_compile_options(${PROJECT_NAME} PRIVATE -march=native)
    endif()
endif()

# =============== Include directories ===============
target_include_directories(${PROJECT_NAME} PRIVATE
        ${SRC_DIR}                          # для #include "core/..."
        ${CMAKE_CURRENT_SOURCE_DIR}/external/glad/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/opensimplex
        ${CMAKE_CURRENT_SOURCE_DIR}/external/stb
        ${CMAKE_CURRENT_SOURCE_DIR}/external/cgltf
)

# =============== Библиотеки ===============
find_package(OpenGL REQUIRED)

# GLFW (ищем системную)
find_package(glfw3 REQUIRED QUIET)

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    find_package(Threads REQUIRED)
endif()

if(OPTICRAFT_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT OPTICRAFT_IPO_OK OUTPUT OPTICRAFT_IPO_LOG)
    if(OPTICRAFT_IPO_OK)
        set_property(TARGET ${PROJECT_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(WARNING "LTO не поддерживается компилятором: ${OPTICRAFT_IPO_LOG}")
    endif()
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenGL::GL
        glfw
        ${X11_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ${CMAKE_DL_LIBS}        # для dlopen (нужен glad на Linux)
)

# =============== Копирование ассетов после сборки ===============
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets
        COMMENT "Копирование папки assets в директорию сборки"
)

# =============== Финальное сообщение ===============
message(STATUS "✅ OptiCraft configured successfully with modern structure!")
