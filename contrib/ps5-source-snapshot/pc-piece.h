#pragma once
#include <stdint.h>
/* Returns a malloc-owned full RGBA image, or NULL to retain the original.
   source_hash/start/span describe the existing PS5 VRAM snapshot. */
unsigned char *sc1_pc_piece(uint64_t source_hash,uint32_t tcw,uint32_t tsp,
    uint32_t start,uint32_t span,uint32_t *width,uint32_t *height);
