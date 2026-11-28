#ifndef GP32_LZMA_H
#define GP32_LZMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Unpacks an LZMA "alone" stream of exactly `size` bytes into `out`. */
bool gp32_lzma_decode(uint8_t *out, size_t size, const uint8_t *in, size_t in_size);

#endif
