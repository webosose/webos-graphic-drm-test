// https://gist.githubusercontent.com/Miouyouyou/89e9fe56a2c59bce7d4a18a858f389ef/raw/b4b5c03398ef84446f9212e1112d87dcaee8fa87/Linux_DRM_OpenGLES.c
// gcc -o drmgl Linux_DRM_OpenGLES.c `pkg-config --cflags --libs libdrm` -lgbm -lEGL -lGLESv2

/*
 * Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 * Copyright (c) 2012 Rob Clark <rob@ti.com>
 * Copyright (c) 2017 Miouyouyou <Myy> <myy@miouyouyou.fr>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Based on a egl cube test app originally written by Arvin Schnell */

#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <drm_fourcc.h>
#include <errno.h>

#include "readpng.h"
#include "drm-common.h"

bool verbose = false;

static struct gbm gbm;
static struct gl gl;

struct plane {
    drmModePlane *plane;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
};

struct crtc {
    drmModeCrtc *crtc;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
};

struct connector {
    drmModeConnector *connector;
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
};

static struct drm drm;

static struct glcolor red = {1.0f, 0.0f, 0.0f, 1.0f};
static struct glcolor blue = {0.0f, 0.0f, 1.0f, 1.0f};
static struct glcolor black = {0.0f, 0.0f, 0.0f, 1.0f};

static const int default_duration = 50;
static char *default_primary_info = "31@1920x1080";
static char *default_overlay_info = "38@512x2160";
static char *default_location = "/usr/share/drmplanes";

static const char *primary_file_name = "primary_1920x1080.png";
static const char *secondary_file_name = "secondary_512x2160.png";

static int default_crtc_width = 3840;
static int default_crtc_height = 2160;

static int init_drm_atomic_planes(uint32_t primary_plane_id, uint32_t overlay_plane_id)
{
    drm.primary_plane = calloc(1, sizeof(*drm.primary_plane));
    drm.overlay_plane = calloc(1, sizeof(*drm.overlay_plane));

#define get_plane_resource(plane, type, Type, id) do {        \
        drm.plane->type = drmModeGet##Type(drm.fd, id); \
        if (!drm.plane->type) {                         \
            printf("could not get %s %i: %s\n",         \
                #type, id, strerror(errno));            \
            return -1;                                  \
        }                                               \
    } while (0)

    get_plane_resource(primary_plane, plane, Plane, primary_plane_id);
    get_plane_resource(overlay_plane, plane, Plane, overlay_plane_id);

#define get_plane_properties(plane, type, TYPE, id) do {                      \
        uint32_t i;                                                     \
        drm.plane->props = drmModeObjectGetProperties(drm.fd,           \
            id, DRM_MODE_OBJECT_##TYPE);                                \
        if (!drm.plane->props) {                                        \
            printf("could not get %s %u properties: %s\n",              \
                #type, id, strerror(errno));                            \
            return -1;                                                  \
        }                                                               \
        drm.plane->props_info = calloc(drm.plane->props->count_props,   \
            sizeof(*drm.plane->props_info));                            \
        for (i = 0; i < drm.plane->props->count_props; i++) {           \
            drm.plane->props_info[i] = drmModeGetProperty(drm.fd,       \
                drm.plane->props->props[i]);                            \
        }                                                               \
    } while (0)

    get_plane_properties(primary_plane, plane, PLANE, primary_plane_id);
    get_plane_properties(overlay_plane, plane, PLANE, overlay_plane_id);

    return 0;
}

static int init_drm_atomic(char *device_path, char *mode_str)
{
    int ret = init_drm(&drm, device_path, mode_str);
    if (ret)
        return ret;

    ret = drmSetClientCap(drm.fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (ret) {
        printf("no atomic modesetting support: %s\n", strerror(errno));
        return ret;
    }

    drm.crtc = calloc(1, sizeof(*drm.crtc));
    drm.connector = calloc(1, sizeof(*drm.connector));

#define get_resource(type, Type, id) do {               \
        drm.type->type = drmModeGet##Type(drm.fd, id);  \
        if (!drm.type->type) {                          \
            printf("could not get %s %i: %s\n",         \
                #type, id, strerror(errno));            \
            return -1;                                  \
        }                                               \
    } while (0)

    get_resource(connector, Connector, drm.connector_id);
    get_resource(crtc, Crtc, drm.crtc_id);

#define get_properties(type, TYPE, id) do {                         \
        uint32_t i;                                                 \
        drm.type->props = drmModeObjectGetProperties(drm.fd,        \
            id, DRM_MODE_OBJECT_##TYPE);                            \
        if (!drm.type->props) {                                     \
            printf("could not get %s %u properties: %s\n",          \
                #type, id, strerror(errno));                        \
            return -1;                                              \
        }                                                           \
        drm.type->props_info = calloc(drm.type->props->count_props, \
            sizeof(*drm.type->props_info));                         \
        for (i = 0; i < drm.type->props->count_props; i++) {        \
            drm.type->props_info[i] = drmModeGetProperty(drm.fd,    \
                drm.type->props->props[i]);                         \
        }                                                           \
    } while (0)

    /*
     * get_properties(plane, PLANE, plane_id);
     */
    get_properties(crtc, CRTC, drm.crtc_id);
    get_properties(connector, CONNECTOR, drm.connector_id);

    return 0;
}

static void print_usage(const char *progname)
{
    printf("Usage:\n");
    printf("    %s -p <plane_id>@<width>x<height> -o <plane_id>@<width>x<height> -v -d <duration> -D <device_path> -m <mode_str>\n", progname);
    printf("\n");
    printf("    -p primary plane info (default: %s)\n", default_primary_info);
    printf("    -o overlay plane info (default: %s)\n", default_overlay_info);
    printf("    -c CRTC resolution to be used drmModeSetPlane for primary plane (default: %dx%d)\n",
        default_crtc_width, default_crtc_height);
    printf("    -v verbose\n");
    printf("    -d duration (default: %d)\n", default_duration);
    printf("    -D drm device path (default: /dev/dri/card0)\n");
    printf("    -m mode preferred (default: NULL, mode with highest resolution)\n");
    printf("    -f FOURCC format (default: AR24)\n");
    printf("    -l resource location (default: /usr/share/drmplanes)\n");
    printf("    -h help\n");
    printf("\n");
    printf("Example:\n");
    printf("    %s -p 31@1920x1080 -o 38@512x2160 -v -d 100 -m 1920x1080 -f AR24 -c 3840x2160\n", progname);
}

static int add_connector_property(drmModeAtomicReq *req, uint32_t obj_id,
                    const char *name, uint64_t value)
{
    struct connector *obj = drm.connector;
    unsigned int i;
    int prop_id = 0;

    for (i = 0 ; i < obj->props->count_props ; i++) {
        if (strcmp(obj->props_info[i]->name, name) == 0) {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }

    if (prop_id < 0) {
        printf("no connector property: %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int add_crtc_property(drmModeAtomicReq *req, uint32_t obj_id,
                const char *name, uint64_t value)
{
    struct crtc *obj = drm.crtc;
    unsigned int i;
    int prop_id = -1;

    for (i = 0 ; i < obj->props->count_props ; i++) {
        if (strcmp(obj->props_info[i]->name, name) == 0) {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }

    if (prop_id < 0) {
        printf("no crtc property: %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int add_plane_property(drmModeAtomicReq *req, uint32_t obj_id,
    const char *name, uint64_t value)
{
    struct plane *obj = drm.primary_plane;
    unsigned int i;
    int prop_id = -1;

    for (i = 0 ; i < obj->props->count_props ; i++) {
        if (strcmp(obj->props_info[i]->name, name) == 0) {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }

    if (prop_id < 0) {
        printf("no plane property: %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static void drm_atomic_set_plane_properties(drmModeAtomicReq *req, uint32_t plane_id,
    uint32_t crtc_id, uint32_t fb_id,
    uint32_t src_width, uint32_t src_height,
    uint32_t crtc_width, uint32_t crtc_height,
    uint32_t crtc_x)
{
    add_plane_property(req, plane_id, "FB_ID", fb_id);
    add_plane_property(req, plane_id, "CRTC_ID", crtc_id);
    add_plane_property(req, plane_id, "SRC_X", 0);
    add_plane_property(req, plane_id, "SRC_Y", 0);
    add_plane_property(req, plane_id, "SRC_W", src_width << 16);
    add_plane_property(req, plane_id, "SRC_H", src_height << 16);
    add_plane_property(req, plane_id, "CRTC_X", crtc_x);
    add_plane_property(req, plane_id, "CRTC_Y", 0);
    add_plane_property(req, plane_id, "CRTC_W", crtc_width);
    add_plane_property(req, plane_id, "CRTC_H", crtc_height);
}

static int drm_atomic_mode_set(drmModeAtomicReq *req, uint32_t flags)
{
    if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
        uint32_t blob_id;

        if (add_connector_property(req, drm.connector_id, "CRTC_ID",
                        drm.crtc_id) < 0)
                return -1;

        if (drmModeCreatePropertyBlob(drm.fd, drm.mode, sizeof(*drm.mode),
                          &blob_id) != 0)
            return -1;

        if (add_crtc_property(req, drm.crtc_id, "MODE_ID", blob_id) < 0)
            return -1;

        if (add_crtc_property(req, drm.crtc_id, "ACTIVE", 1) < 0)
            return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    struct gbm_bo *bo = NULL, *bo_next = NULL;
    struct drm_fb *fb = NULL;
    uint32_t i = 0;
    int ret;

    int opt;
    int duration = default_duration;
    int primary_plane_id = 31;
    int overlay_plane_id = 38;
    char *primary_plane_info = default_primary_info;
    char *overlay_plane_info = default_overlay_info;
    char *device_path = "/dev/dri/card0";
    char *mode_str = NULL;
    char *crtc_str = NULL;
    uint32_t format = GBM_FORMAT_ARGB8888;
    char *location = default_location;

    while ((opt = getopt(argc, argv, "hvad:p:o:D:m:f:l:c:")) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'c':
                crtc_str = optarg;
                break;
            case 'l':
                location = optarg;
                break;
            case 'm':
                mode_str = optarg;
                break;
            case 'd':
                duration = strtoul(optarg, NULL, 10);
                break;
            case 'v':
                verbose = true;
                break;
            case 'p':
                primary_plane_info = optarg;
                break;
            case 'o':
                overlay_plane_info = optarg;
                break;
            case 'D':
                device_path = optarg;
                break;
            case 'f': {
                char fourcc[4] = "    ";
                int length = strlen(optarg);
                if (length > 0)
                    fourcc[0] = optarg[0];
                if (length > 1)
                    fourcc[1] = optarg[1];
                if (length > 2)
                    fourcc[2] = optarg[2];
                if (length > 3)
                    fourcc[3] = optarg[3];
                format = fourcc_code(fourcc[0], fourcc[1],
                    fourcc[2], fourcc[3]);
                break;
            }
            case '?':
                if (optopt == 'p' || optopt == 'o')
                    fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                else if (isprint(optopt))
                    fprintf(stderr, "Unknown option `-%c'.\n", optopt);
                else
                    fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                return 1;

                break;
            default:
                abort();
        }
    }

    int p_w, p_h;
    int o_w, o_h;
    if (!parse_plane(primary_plane_info, &primary_plane_id, &p_w, &p_h)) {
        printf("failed to parse primary resolution %s\n", primary_plane_info);
        return ret;
    }

    if (!parse_plane(overlay_plane_info, &overlay_plane_id, &o_w, &o_h)) {
        printf("failed to parse overlay resolution %s\n", overlay_plane_info);
        return ret;
    }

    ret = init_drm_atomic(device_path, mode_str);
    if (ret) {
        printf("failed to initialize DRM\n");
        return ret;
    }

    LOG_ARGS("drm->mode: %dx%d\n", drm.mode->hdisplay, drm.mode->vdisplay);

    ret = init_drm_atomic_planes(primary_plane_id, overlay_plane_id);
    if (ret) {
        printf("failed to initialize atomic planes\n");
        return ret;
    }

    ret = init_gbm(&gbm, drm.fd, p_w, p_h, o_w, o_h, format);
    if (ret) {
        printf("failed to initialize GBM\n");
        return ret;
    }

    ret = init_gl(&gl, &gbm, format);
    if (ret) {
        printf("failed to initialize EGL\n");
        return ret;
    }

    uint32_t plane_flags = 0;

    struct gbm_bo *bo2 = NULL, *bo2_next = NULL;
    struct drm_fb *fb2 = NULL;

    /* surface1 */
    if (!add_surface(drm.fd, &gl, &gbm, gl.surface1, gbm.surface1, &red, &bo, &fb)) {
        fprintf(stderr, "fail to add surface 1\n");
        return 1;
    }
    if (!add_surface(drm.fd, &gl, &gbm, gl.surface2, gbm.surface2, &blue, &bo2, &fb2)) {
        fprintf(stderr, "fail to add surface 2\n");
        return 1;
    }

    char filepath[1024];
    memset(filepath, '\0', sizeof filepath);

    png_buffer_handle png_handle_primary;
    get_resource_path(filepath, location, primary_file_name);
    if (!read_png_from_file(filepath, &png_handle_primary)) {
        fprintf(stderr, "fail to read_png_from_file %s\n", primary_file_name);
        return 1;
    }
    if (!fill_gbm_buffer(bo, png_handle_primary)) {
        fprintf(stderr, "fail to fill primary bo\n");
        return 1;
    }

    png_buffer_handle png_handle_secondary;
    get_resource_path(filepath, location, secondary_file_name);
    if (!read_png_from_file(filepath, &png_handle_secondary)) {
        fprintf(stderr, "fail to read_png_from_file %s\n", secondary_file_name);
        return 1;
    }
    if (!fill_gbm_buffer(bo2, png_handle_secondary)) {
        fprintf(stderr, "fail to fill secondary bo\n");
        return 1;
    }

    int crtc_width = default_crtc_width;
    int crtc_height = default_crtc_height;

    if (crtc_str) {
        if (!parse_resolution(crtc_str, &crtc_width, &crtc_height)) {
            fprintf(stderr, "failed to parse crtc_str: %s\n", crtc_str);
            return -1;
        }
    }
    printf("CRTC width: %d height: %d\n", crtc_width, crtc_height);

    bool turn_overlay_on = false;
    bool turn_primary_on = false;

    int j = 0;

    uint32_t flags = 0;
    flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    /* TODO: Support non-blocking commit */
    /*
     * flags |= DRM_MODE_ATOMIC_NONBLOCK;
     */

    while (true) {
        int x_offset = (j * 10) % crtc_width;
        int waiting_for_flip = 1;
        bool overlay_visible = false;
        bool prev_cond = false;

        i++;

        /*
         * xor primary and overlay for duration
         * primary => 1..duration/2
         * overlay => duration/2..duration
         */
        overlay_visible = (i % duration) > (duration / 2);
        prev_cond = ((i-1) % duration) > (duration / 2);

        turn_overlay_on = !prev_cond && overlay_visible;
        turn_primary_on = (prev_cond && !overlay_visible) || i == 1;

        eglMakeCurrent(gl.display, gl.surface1, gl.surface1, gl.context);
        eglSwapBuffers(gl.display, gl.surface1);

        drmModeAtomicReq *req;
        req = drmModeAtomicAlloc();

        drm_atomic_mode_set(req, flags);

        if (turn_primary_on) {
            drm_atomic_set_plane_properties(req, primary_plane_id, drm.crtc_id, fb->fb_id,
                p_w, p_h, crtc_width, crtc_height, 0);
            drm_atomic_set_plane_properties(req, overlay_plane_id, 0, 0,
                0, 0, 0, 0, 0);
        }

        if (turn_overlay_on)
            drm_atomic_set_plane_properties(req, primary_plane_id, 0, 0,
                0, 0, 0, 0, 0);

        if (overlay_visible) {
            drm_atomic_set_plane_properties(req, overlay_plane_id, drm.crtc_id, fb2->fb_id,
                o_w, o_h, o_w, o_h, x_offset);
            j++;
        }

        ret = drmModeAtomicCommit(drm.fd, req, flags, NULL);
        LOG_ARGS("%i: drmModeAtomicCommit(%d %p %x) returns %d(%s)\n", i, drm.fd, req, flags, ret, strerror(ret));

        drmModeAtomicFree(req);

        if (bo)
            release_gbm_bo(&gbm, gbm.surface1, bo);
        bo = bo_next;
        bo_next = NULL;

        if (bo2)
            release_gbm_bo(&gbm, gbm.surface2, bo2);
        bo2 = bo2_next;
        bo2_next = NULL;
    }

    destroy_png_buffer(png_handle_primary);
    destroy_png_buffer(png_handle_secondary);

    return 0;
}
