// Заглушки для сборки генератора без рендера и менеджера чанков: эти функции нужны Chunk.cpp
// для мешинга соседей, а генератор их не вызывает.
#include "world/chunks/Chunk_Manager.h"
#include <cstdlib>

Block_Types Chunk_Manager::get_block_world(int, int, int) const { return Block_Types::Air; }
uint8_t Chunk_Manager::get_block_meta_world(int, int, int) const { return 0; }
uint8_t Chunk_Manager::get_light_world(int, int, int) const { return 0; }
