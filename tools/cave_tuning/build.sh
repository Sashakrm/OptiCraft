#!/usr/bin/env bash
# Собирает cave_tune отдельно от движка — использует только реальный шум движка
# (../../external/opensimplex/OpenSimplex2S.c), без Chunk/Renderer/OpenGL и прочих зависимостей.
set -euo pipefail
cd "$(dirname "$0")"

SRC_NOISE=../../external/opensimplex/OpenSimplex2S.c
gcc -c -O2 -I../../external/opensimplex "$SRC_NOISE" -o OpenSimplex2S.o
g++ -std=c++17 -O2 -I../../external/opensimplex cave_tune.cpp OpenSimplex2S.o -o cave_tune -lm

echo "Собрано: tools/cave_tuning/cave_tune"
