cmake_minimum_required(VERSION 3.10)
project(OptiCraft)

# =============== Настройки ===============
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Удобно для IDE (VS Code, CLion и т.д.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============== Пути ===============
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# =============== Все исходные файлы ===============
set(SOURCES
        main.cpp

        # core
        ${SRC_DIR}/core/Application.cpp
        ${SRC_DIR}/core/Game.cpp

        # entities
        ${SRC_DIR}/entities/Camera.cpp
        ${SRC_DIR}/entities/Player.cpp

        # input
        ${SRC_DIR}/input/Input_Manager.cpp

        # rendering
        ${SRC_DIR}/rendering/Mesh.cpp
        ${SRC_DIR}/rendering/Renderer.cpp
        ${SRC_DIR}/rendering/Shader.cpp
        ${SRC_DIR}/rendering/Texture_Atlas.cpp

        # utils
        ${SRC_DIR}/utils/Logger.cpp

        # world
        ${SRC_DIR}/world/Block.cpp
        ${SRC_DIR}/world/Chunk.cpp
        ${SRC_DIR}/world/Chunk_Manager.cpp
        ${SRC_DIR}/world/World_Generator.cpp
)

# Glad (отдельная статическая библиотека)
set(GLAD_SRC ${CMAKE_CURRENT_SOURCE_DIR}/external/glad/src/gl.c
        src/core/Events.cpp
        src/core/Events.h
        src/core/Window.cpp
        src/core/Window.h
        src/world/Block_Types.cpp
        src/utils/Hash_Utils.h
        src/utils/Texture.cpp
        src/utils/Texture.h
        src/world/Biome_Types.cpp
        src/world/Biome_Types.h
        src/world/Biome_Generator.cpp
        src/world/Biome_Generator.h
        src/utils/Noise.cpp
        src/utils/Noise.h)

# =============== Создаём исполняемый файл ===============
add_executable(${PROJECT_NAME} ${SOURCES} ${GLAD_SRC})

# Добавляем заголовочные файлы (чтобы они отображались в IDE)
target_sources(${PROJECT_NAME} PRIVATE
        ${SRC_DIR}/core/Application.h
        ${SRC_DIR}/core/Game.h

        ${SRC_DIR}/entities/Camera.h
        ${SRC_DIR}/entities/Player.h

        ${SRC_DIR}/input/Input_Call_Backs.h
        ${SRC_DIR}/input/Input_Manager.h

        ${SRC_DIR}/rendering/Mesh.h
        ${SRC_DIR}/rendering/Renderer.h
        ${SRC_DIR}/rendering/Shader.h
        ${SRC_DIR}/rendering/Texture_Atlas.h

        ${SRC_DIR}/utils/Config.h
        ${SRC_DIR}/utils/Logger.h
        ${SRC_DIR}/utils/Math_Helpers.h

        ${SRC_DIR}/world/Block.h
        ${SRC_DIR}/world/Block_Types.h
        ${SRC_DIR}/world/Chunk.h
        ${SRC_DIR}/world/Chunk_Manager.h
        ${SRC_DIR}/world/World_Generator.h
)

# =============== Include directories ===============
target_include_directories(${PROJECT_NAME} PRIVATE
        ${SRC_DIR}                          # для #include "core/..."
        ${CMAKE_CURRENT_SOURCE_DIR}/external/glad/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/stb
)

# =============== Библиотеки ===============
find_package(OpenGL REQUIRED)

# GLFW (ищем системную)
find_package(glfw3 REQUIRED QUIET)

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    find_package(Threads REQUIRED)
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