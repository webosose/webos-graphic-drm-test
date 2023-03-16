#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drm-common.h"

static struct {
    struct egl egl;

    /* TODO: call destroy_png_buffer */
    png_buffer_handle handle_primary;
    png_buffer_handle handle_secondary;
} gl;

static void draw_png(unsigned i, struct gbm_bo *bo, bool is_primary)
{
    if (!fill_gbm_buffer(bo, is_primary ? gl.handle_primary : gl.handle_secondary)) {
        fprintf(stderr, "fail to fill gbm bo\n");
        return;
    }
}

const struct egl * init_png_image(int drm_fd, const struct gbm *gbm, uint32_t format,
    const char* primary_path, const char* secondary_path)
{
    int ret;

    memset(&gl, 0x0, sizeof(gl));

    ret = init_egl(&gl.egl, gbm, format);
    if (ret)
        return NULL;

    gl.egl.draw = draw_png;

    if (!read_png_from_file(primary_path, &gl.handle_primary)) {
        fprintf(stderr, "fail to read_png_from_file %s\n", primary_path);
        return NULL;
    }
    if (!read_png_from_file(secondary_path, &gl.handle_secondary)) {
        fprintf(stderr, "fail to read_png_from_file %s\n", secondary_path);
        return NULL;
    }

    return &gl.egl;
}
