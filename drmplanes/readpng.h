#ifndef READPNG_H
#define READPNG_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

struct png_buffer;

typedef struct png_buffer* png_buffer_handle;

bool read_png(FILE *fp, unsigned int sig_read, png_buffer_handle *out_png_buffer_handle);
bool fill_buffer(void* addr, uint32_t width, uint32_t height, uint32_t stride, png_buffer_handle png_buffer_handle);
void destroy_png_buffer(png_buffer_handle png_buffer_handle);

#endif /* READPNG_H */
