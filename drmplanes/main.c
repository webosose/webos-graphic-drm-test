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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <assert.h>
#include <drm_fourcc.h>
#include <ctype.h>

#include <drm_fourcc.h>
#include <stdarg.h>

#include "readpng.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))


static bool verbose = false;

static void log_message_with_args(const char *msg, ...) {
    if (!verbose)
        return;

    va_list args;
    va_start(args, msg);
    vprintf(msg, args);
    va_end(args);
}

#define LOG(msg)                                                        \
    log_message_with_args("%s:%d: " msg, __PRETTY_FUNCTION__, __LINE__)

#define LOG_ARGS(msg, ...)                                              \
    log_message_with_args("%s:%d: " msg, __PRETTY_FUNCTION__, __LINE__, __VA_ARGS__)

static struct {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface1;
    EGLSurface surface2;
} gl;

static struct {
    struct gbm_device *dev;
    struct gbm_surface *surface1;
    struct gbm_surface *surface2;
} gbm;

static struct {
    int fd;
    drmModeModeInfo *mode;
    uint32_t crtc_id;
    uint32_t connector_id;
} drm;

struct drm_fb {
    struct gbm_bo *bo;
    uint32_t fb_id;
};

struct glcolor {
    GLclampf r;
    GLclampf g;
    GLclampf b;
    GLclampf a;
};

static struct glcolor red = {1.0f, 0.0f, 0.0f, 1.0f};
static struct glcolor blue = {0.0f, 0.0f, 1.0f, 1.0f};
static struct glcolor black = {0.0f, 0.0f, 0.0f, 1.0f};

static const int default_duration = 50;
static char *default_primary_info = "31@1920x1080";
static char *default_overlay_info = "38@512x1080";
static char *default_location = "/usr/share/drmplanes";

static const char *primary_file_name = "primary_1920x1080.png";
static const char *secondary_file_name = "secondary_512x2160.png";

static int default_crtc_width = 3840;
static int default_crtc_height = 2160;

static char* color_name(struct glcolor *c)
{
    if (c == &red)
        return "red";
    else if (c == &blue)
        return "blue";
    else if (c == &black)
        return "black";
    else
        return "unknown";
}

static uint32_t find_crtc_for_encoder(const drmModeRes *resources,
    const drmModeEncoder *encoder) {
    int i;

    for (i = 0; i < resources->count_crtcs; i++) {
        /* possible_crtcs is a bitmask as described here:
         * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
         */
        const uint32_t crtc_mask = 1 << i;
        const uint32_t crtc_id = resources->crtcs[i];
        if (encoder->possible_crtcs & crtc_mask) {
            return crtc_id;
        }
    }

    /* no match found */
    return -1;
}

static uint32_t find_crtc_for_connector(const drmModeRes *resources,
    const drmModeConnector *connector) {
    int i;

    for (i = 0; i < connector->count_encoders; i++) {
        const uint32_t encoder_id = connector->encoders[i];
        drmModeEncoder *encoder = drmModeGetEncoder(drm.fd, encoder_id);

        if (encoder) {
            const uint32_t crtc_id = find_crtc_for_encoder(resources, encoder);

            drmModeFreeEncoder(encoder);
            if (crtc_id != 0) {
                return crtc_id;
            }
        }
    }

    /* no match found */
    return -1;
}

static bool parse_resolution(char* resolution, int *w, int *h)
{
    char *p = resolution;
    int v = 0;
    bool found_x = false;

    while(*p) {
        if (isdigit(*p)) { // >= '0' && *p <= '9') {
            v *= 10;
            v += *p - '0';
        } else if (*p == 'x') {
            *w = v;
            v = 0;
            found_x = true;
        } else {
            return false;
        }
        p++;
    }
    if (!found_x)
        return false;

    *h = v;
    return true;
}

static int init_drm(char *device_path, char *mode_str)
{
    drmModeRes *resources;
    drmModeConnector *connector = NULL;
    drmModeEncoder *encoder = NULL;
    int i, area;

    drm.fd = open(device_path, O_RDWR);

    if (drm.fd < 0) {
        printf("could not open drm device\n");
        return -1;
    }

    resources = drmModeGetResources(drm.fd);
    if (!resources) {
        printf("drmModeGetResources failed: %s\n", strerror(errno));
        return -1;
    }

    /* find a connected connector: */
    for (i = 0; i < resources->count_connectors; i++) {
        connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
        if (connector->connection == DRM_MODE_CONNECTED) {
            /* it's connected, let's use this! */
            break;
        }
        drmModeFreeConnector(connector);
        connector = NULL;
    }

    if (!connector) {
        /* we could be fancy and listen for hotplug events and wait for
         * a connector..
         */
        printf("no connected connector!\n");
        return -1;
    }

    /* find prefered mode or the highest resolution mode: */
    for (i = 0, area = 0; i < connector->count_modes; i++) {
        drmModeModeInfo *current_mode = &connector->modes[i];

        if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
            drm.mode = current_mode;
        }

        int current_area = current_mode->hdisplay * current_mode->vdisplay;
        if (current_area > area) {
            drm.mode = current_mode;
            area = current_area;
        }
    }

    if (mode_str) {
        int preferred_width;
        int preferred_height;

        if (!parse_resolution(mode_str, &preferred_width, &preferred_height)) {
            printf("failed to parse mode_str: %s\n", mode_str);
            return -1;
        }

        for (i = 0; i < connector->count_modes; i++) {
            drmModeModeInfo *current_mode = &connector->modes[i];

            if (preferred_width == current_mode->hdisplay &&
                preferred_height == current_mode->vdisplay) {
                printf("override matched mode for %dx%d\n", preferred_width, preferred_height);
                drm.mode = current_mode;
                break;
            }
        }
    }

    if (!drm.mode) {
        printf("could not find mode!\n");
        return -1;
    }

    /* find encoder: */
    for (i = 0; i < resources->count_encoders; i++) {
        encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
        if (encoder->encoder_id == connector->encoder_id)
            break;
        drmModeFreeEncoder(encoder);
        encoder = NULL;
    }

    if (encoder) {
        drm.crtc_id = encoder->crtc_id;
    } else {
        uint32_t crtc_id = find_crtc_for_connector(resources, connector);
        if (crtc_id == 0) {
            printf("no crtc found!\n");
            return -1;
        }

        drm.crtc_id = crtc_id;
    }

    drm.connector_id = connector->connector_id;

    return 0;
}

static int init_gbm(int p_w, int p_h, int o_w, int o_h, uint32_t format)
{
    printf("init_gbm: primary: %dx%d overlay: %dx%d\n", p_w, p_h, o_w, o_h);

    gbm.dev = gbm_create_device(drm.fd);

    uint64_t modifier = DRM_FORMAT_MOD_LINEAR;

    gbm.surface1 = gbm_surface_create_with_modifiers(gbm.dev, p_w, p_h,
        format, &modifier, 1);
    LOG("gbm.surface1 created by gbm_surface_create_with_modifiers\n");
    if (!gbm.surface1) {
        gbm.surface1 = gbm_surface_create(gbm.dev, p_w, p_h, format,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!gbm.surface1) {
            printf("failed to create gbm surface1\n");
            return -1;
        }
    }

    gbm.surface2 = gbm_surface_create_with_modifiers(gbm.dev, o_w, o_h,
        format, &modifier, 1);
    LOG("gbm.surface2 created by gbm_surface_create_with_modifiers\n");
    if (!gbm.surface2) {
        gbm.surface2 = gbm_surface_create(gbm.dev, o_w, o_h, format,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!gbm.surface2) {
            printf("failed to create gbm surface2\n");
            return -1;
        }
    }
    return 0;
}

static int
match_config_to_visual(EGLDisplay egl_display,
    EGLint visual_id,
    EGLConfig *configs,
    int count)
{
    int i;

    for (i = 0; i < count; ++i) {
        EGLint id;

        if (!eglGetConfigAttrib(egl_display,
                configs[i], EGL_NATIVE_VISUAL_ID,
                &id))
            continue;

        printf("configs[%d] = 0x%08x(%c%c%c%c), visual=0x%08x(%c%c%c%c)\n",
            i,
            id,  (id>>0)&0xff, (id>>8)&0xff, (id>>16)&0xff, (id>>24)&0xff,
            visual_id, (visual_id>>0)&0xff, (visual_id>>8)&0xff, (visual_id>>16)&0xff, (visual_id>>24)&0xff);

        if (id == visual_id)
            return i;
    }

    return -1;
}

static bool
egl_choose_config(EGLDisplay egl_display, const EGLint *attribs,
    EGLint visual_id, EGLConfig *config_out)
{
    EGLint count = 0;
    EGLint matched = 0;
    EGLConfig *configs;
    int config_index = -1;

    if (!eglGetConfigs(egl_display, NULL, 0, &count) || count < 1) {
        printf("No EGL configs to choose from.\n");
        return false;
    }
    configs = malloc(count * sizeof *configs);
    if (!configs)
        return false;

    if (!eglChooseConfig(egl_display, attribs, configs,
            count, &matched) || !matched) {
        printf("No EGL configs with appropriate attributes.\n");
        goto out;
    }

    if (!visual_id)
        config_index = 0;

    if (config_index == -1)
        config_index = match_config_to_visual(egl_display,
            visual_id,
            configs,
            matched);

    if (config_index != -1)
        *config_out = configs[config_index];

out:
    free(configs);
    if (config_index == -1)
        return false;

    return true;
}

static int init_gl(uint32_t format)
{
    EGLint major, minor, n;
    GLuint vertex_shader, fragment_shader;
    GLint ret;

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
    get_platform_display =
            (void *) eglGetProcAddress("eglGetPlatformDisplayEXT");
    assert(get_platform_display != NULL);

    gl.display = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm.dev, NULL);

    if (!eglInitialize(gl.display, &major, &minor)) {
        printf("failed to initialize\n");
        return -1;
    }

    printf("Using display %p with EGL version %d.%d\n",
        gl.display, major, minor);

    printf("EGL Version \"%s\"\n", eglQueryString(gl.display, EGL_VERSION));
    printf("EGL Vendor \"%s\"\n", eglQueryString(gl.display, EGL_VENDOR));
    printf("EGL Extensions \"%s\"\n", eglQueryString(gl.display, EGL_EXTENSIONS));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        printf("failed to bind api EGL_OPENGL_ES_API\n");
        return -1;
    }

    if (!egl_choose_config(gl.display, config_attribs, format,
            &gl.config)) {
        printf("failed to choose config\n");
        return -1;
    }

    gl.context = eglCreateContext(gl.display, gl.config,
        EGL_NO_CONTEXT, context_attribs);
    if (gl.context == NULL) {
        printf("failed to create context 1\n");
        return -1;
    }

    gl.surface1 = eglCreateWindowSurface(gl.display, gl.config, gbm.surface1, NULL);
    if (gl.surface1 == EGL_NO_SURFACE) {
        printf("failed to create egl surface 1\n");
        return -1;
    }

    gl.surface2 = eglCreateWindowSurface(gl.display, gl.config, gbm.surface2, NULL);
    if (gl.surface2 == EGL_NO_SURFACE) {
        printf("failed to create egl surface 2\n");
        return -1;
    }

    /* connect the context to the surface */
    eglMakeCurrent(gl.display, gl.surface1, gl.surface1, gl.context);

    printf("GL Extensions: \"%s\"\n", glGetString(GL_EXTENSIONS));

    return 0;
}

static char* surface_name(EGLSurface surface)
{
    if (surface == gl.surface1)
        return "primary";
    else if (surface == gl.surface2)
        return "overlay";
    else
        return "unknown";
}

static char* gbm_surface_name(struct gbm_surface *surface)
{
    if (surface == gbm.surface1)
        return "primary";
    else if (surface == gbm.surface2)
        return "overlay";
    else
        return "unknown";
}

static void draw_gl(int i, struct glcolor *color, EGLSurface gl_surface)
{
    LOG_ARGS("%3d: fill color %s on %s\n", i, color_name(color), surface_name(gl_surface));
    glClearColor(color->r, color->g, color->b, color->a);
	glClear(GL_COLOR_BUFFER_BIT);
}

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
    struct drm_fb *fb = data;
    struct gbm_device *gbm = gbm_bo_get_device(bo);

    if (fb->fb_id) {
        drmModeRmFB(drm.fd, fb->fb_id);
        LOG_ARGS("drmModeRmFB fb_id: %d\n", fb->fb_id);
    }

    free(fb);
}

static struct drm_fb * drm_fb_get_from_bo(struct gbm_bo *bo)
{
    struct drm_fb *fb = gbm_bo_get_user_data(bo);
    uint32_t width, height, stride, handle, format;
    int ret;

    if (fb)
        return fb;

    fb = calloc(1, sizeof *fb);
    fb->bo = bo;

    width = gbm_bo_get_width(bo);
    height = gbm_bo_get_height(bo);
    stride = gbm_bo_get_stride(bo);
    handle = gbm_bo_get_handle(bo).u32;
    format = gbm_bo_get_format(bo);

    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 };
    uint32_t strides[4] = { gbm_bo_get_stride(bo) };
    uint32_t offsets[4] = { 0 };

    ret = drmModeAddFB2(drm.fd, width, height, format,
        handles, strides, offsets, &fb->fb_id, 0);
    LOG_ARGS("drmModeAddFB2(%d, %d) fb_id: %d\n", width, height, fb->fb_id);

    if (ret) {
        printf("failed to create fb: %s\n", strerror(errno));
        free(fb);
        return NULL;
    }

    gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

    return fb;
}

static void page_flip_handler(int fd, unsigned int frame,
    unsigned int sec, unsigned int usec, void *data)
{
    int *waiting_for_flip = data;
    *waiting_for_flip = 0;
}

static bool parse_plane(char* resolution, int *id, int *w, int *h)
{
    char *p = resolution;
    int v = 0;
    bool found_x = false;
    bool found_id = false;

    while(*p) {
        if (isdigit(*p)) { // >= '0' && *p <= '9') {
            v *= 10;
            v += *p - '0';
        } else if (*p == '@') {
            *id = v;
            v = 0;
            found_id = true;
        } else if (*p == 'x') {
            *w = v;
            v = 0;
            found_x = true;
        } else {
            return false;
        }
        p++;
    }
    if (!found_id || !found_x)
        return false;

    *h = v;
    return true;
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
    printf("    -w fill black workaround (instead of turning off primary plane)\n");
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

static bool lock_new_surface(struct gbm_surface *gbm_surface, struct gbm_bo **out_bo, struct drm_fb **out_fb)
{
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm_surface);
    if (!bo) {
        fprintf(stderr, "fail to lock front buffer(%s)\n", strerror(errno));
        return false;
    }
    LOG_ARGS("gbm_surface_lock_front_buffer: %s %p\n", gbm_surface_name(gbm_surface), bo);

    *out_bo = bo;
    struct drm_fb *fb = drm_fb_get_from_bo(bo);
    if (!fb) {
        fprintf(stderr, "fail to get fb from bo(%s)\n", strerror(errno));
        return false;
    }
    *out_fb = fb;
    return true;
}

static bool read_png_from_file(const char* filename, png_buffer_handle *out_png_buffer_handle)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("failed to open %s\n", filename);
        return false;
    }
    unsigned int sig_read = 0;
    png_buffer_handle png_buffer_handle = NULL;
    bool ret = read_png(fp, sig_read, &png_buffer_handle);
    if (!ret) {
        printf("failed to read_png %s\n", filename);
        fclose(fp);
        return false;
    }
    *out_png_buffer_handle = png_buffer_handle;
    return true;
}

static bool fill_gbm_buffer(struct gbm_bo *bo, png_buffer_handle png_buffer_handle)
{
    uint32_t stride;
    uint8_t *addr = NULL;
    void *mmap_data = NULL;

    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);

    addr = gbm_bo_map(bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &stride, &mmap_data);
    if (!addr) {
        printf("failed to map surface\n");
        return false;
    }

    if (!fill_buffer(addr, width, height, stride, png_buffer_handle)) {
        printf("failed to fill_buffer\n");
        return false;
    }

    gbm_bo_unmap(bo, mmap_data);
    return true;
}

static bool read_png_and_write_to_bo(const char* filename, struct gbm_bo *bo)
{
    uint32_t stride;
    uint8_t *addr = NULL;
    void *mmap_data = NULL;

    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);

    addr = gbm_bo_map(bo, 0, 0, width, height, 0, &stride, &mmap_data);
    if (!addr) {
        printf("failed to map surface\n");
        return false;
    }

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("failed to open %s\n", filename);
        return false;
    }
    unsigned int sig_read = 0;
    png_buffer_handle png_buffer_handle = NULL;
    bool ret = read_png(fp, sig_read, &png_buffer_handle);
    if (!ret) {
        printf("failed to read_png %s\n", filename);
        fclose(fp);
        return false;
    }
    if (!fill_buffer(addr, width, height, stride, png_buffer_handle)) {
        printf("failed to fill_buffer\n");
        fclose(fp);
        return false;
    }

    gbm_bo_unmap(bo, mmap_data);
    return true;
}

static bool add_surface(EGLSurface gl_surface, struct gbm_surface *gbm_surface, struct glcolor *color,
    struct gbm_bo **out_bo, struct drm_fb **out_fb)
{
    eglMakeCurrent(gl.display, gl_surface, gl_surface, gl.context);
    /*
     * draw_gl(0, color, gl_surface);
     */
    eglSwapBuffers(gl.display, gl_surface);
    return lock_new_surface(gbm_surface, out_bo, out_fb);
}

static void release_gbm_bo(struct gbm_surface* surface, struct gbm_bo *bo)
{
    if (bo) {
        LOG_ARGS("gbm_surface_release_buffer: %s %p\n", gbm_surface_name(surface), bo);
        gbm_surface_release_buffer(surface, bo);
    }
}

void get_resource_path(char* fullpath, const char *location, const char *filename)
{
    fullpath[0] = '\0';
    strcat(fullpath, location);
    strcat(fullpath, "/");
    strcat(fullpath, filename);
}

int main(int argc, char *argv[])
{
    fd_set fds;
    drmEventContext evctx = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler = page_flip_handler,
    };
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

    bool fill_black_workaround = false;

    while ((opt = getopt(argc, argv, "whvd:p:o:D:m:f:l:c:")) != -1) {
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
            case 'w':
                fill_black_workaround = true;
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

    ret = init_drm(device_path, mode_str);
    if (ret) {
        printf("failed to initialize DRM\n");
        return ret;
    }

    LOG_ARGS("drm->mode: %dx%d\n", drm.mode->hdisplay, drm.mode->vdisplay);

    FD_ZERO(&fds);
    FD_SET(0, &fds);
    FD_SET(drm.fd, &fds);

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

    ret = init_gbm(p_w, p_h, o_w, o_h, format);
    if (ret) {
        printf("failed to initialize GBM\n");
        return ret;
    }

    ret = init_gl(format);
    if (ret) {
        printf("failed to initialize EGL\n");
        return ret;
    }

    uint32_t plane_flags = 0;

    /* surface1 */
    if (!add_surface(gl.surface1, gbm.surface1, &red, &bo, &fb)) {
        fprintf(stderr, "fail to add surface 1\n");
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

    /* set mode: */
    ret = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0,
            &drm.connector_id, 1, drm.mode);
    if (ret) {
        printf("failed to set mode: %s\n", strerror(errno));
        return ret;
    }

    struct gbm_bo *bo2 = NULL, *bo2_next = NULL;
    struct drm_fb *fb2 = NULL;

    int crtc_width = default_crtc_width;
    int crtc_height = default_crtc_height;

    if (crtc_str) {
        if (!parse_resolution(crtc_str, &crtc_width, &crtc_height)) {
            fprintf(stderr, "failed to parse crtc_str: %s\n", crtc_str);
            return -1;
        }
    }
    printf("CRTC width: %d height: %d\n", crtc_width, crtc_height);

    /* no difference w/ or w/o this */
    LOG_ARGS("%3d: drmModeSetPlane(%d, %d, %d, %d, ..., %d, %d, ..., %d << 16, %d << 16)\n", i,
        drm.fd, primary_plane_id, drm.crtc_id, fb->fb_id,
        crtc_width, crtc_height, p_w, p_h);
    ret = drmModeSetPlane(drm.fd, primary_plane_id, drm.crtc_id, fb->fb_id,
        plane_flags, 0, 0, crtc_width, crtc_height, 0, 0, p_w << 16, p_h << 16);
    if (ret)
        fprintf(stderr, "failed to set plane(primary) on: %s. Keep going anyway\n", strerror(errno));

    LOG_ARGS("%3d: drmModeSetPlane primary on\n", i);

    bool turn_overlay_on = false;
    bool turn_primary_on = false;

    int j = 0;

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
        turn_primary_on = prev_cond && !overlay_visible;

        if (turn_overlay_on) {
            LOG_ARGS("%3d: turn_overlay_on\n", i);
            if (!fb2) {
                if (!add_surface(gl.surface2, gbm.surface2, &blue, &bo2, &fb2)) {
                    fprintf(stderr, "fail to add surface 2\n");
                    return 1;
                }
                if (!fill_gbm_buffer(bo2, png_handle_secondary)) {
                    fprintf(stderr, "fail to fill secondary bo\n");
                    return 1;
                }
            } else {
                eglMakeCurrent(gl.display, gl.surface2, gl.surface2, gl.context);
                /*
                 * draw_gl(i, &blue, gl.surface2);
                 */
                eglSwapBuffers(gl.display, gl.surface2);

                if (!lock_new_surface(gbm.surface2, &bo2_next, &fb2)) {
                    fprintf(stderr, "fail to lock surface 2\n");
                    return 1;
                }
                if (!fill_gbm_buffer(bo2_next, png_handle_secondary)) {
                    fprintf(stderr, "fail to fill secondary bo\n");
                    return 1;
                }
            }

            /*
             * TODO: If we disable the plane of primary id, then page flip fails.
             * Hence, fill black instead
             */
            if (fill_black_workaround) {
                eglMakeCurrent(gl.display, gl.surface1, gl.surface1, gl.context);
                draw_gl(i, &black, gl.surface1);
                eglSwapBuffers(gl.display, gl.surface1);
            } else {
                LOG_ARGS("%3d: drmModeSetPlane primary off\n", i);
                ret = drmModeSetPlane(drm.fd, primary_plane_id, drm.crtc_id, 0,
                    0, 0, 0, 0, 0, 0, 0, 0, 0);
                if (ret)
                    fprintf(stderr, "failed to turn primary plane off(%s)\n", strerror(errno));
            }
        }

        if (overlay_visible) {
            /* move overlay plane */
            j++;
            ret = drmModeSetPlane(drm.fd, overlay_plane_id, drm.crtc_id, fb2->fb_id,
                plane_flags, x_offset, 0, o_w, o_h, 0, 0, o_w << 16, o_h << 16);
            if (ret)
                fprintf(stderr, "failed to set plane(overlay) on: %s\n", strerror(errno));
            else
                LOG_ARGS("%3d: drmModeSetPlane overlay (move overlay to (%d, %d))\n", i, x_offset, 0);
        } else {
            LOG_ARGS("%3d: show primary\n", i);
        }

        if (turn_primary_on) {
            LOG_ARGS("%3d: turn_primary_on\n", i);
            eglMakeCurrent(gl.display, gl.surface1, gl.surface1, gl.context);
            /*
             * draw_gl(i, &red, gl.surface1);
             */
            eglSwapBuffers(gl.display, gl.surface1);

            if (!lock_new_surface(gbm.surface1, &bo_next, &fb)) {
                fprintf(stderr, "fail to lock surface 1\n");
                return 1;
            }

            if (!fill_gbm_buffer(bo_next, png_handle_primary)) {
                fprintf(stderr, "fail to fill primary bo\n");
                return 1;
            }

            LOG_ARGS("%3d: drmModeSetPlane(%d, %d, %d, %d, ..., %d, %d, ..., %d << 16, %d << 16)\n",
                i,
                drm.fd, primary_plane_id, drm.crtc_id, fb->fb_id,
                crtc_width, crtc_height, p_w, p_h);
            ret = drmModeSetPlane(drm.fd, primary_plane_id, drm.crtc_id, fb->fb_id,
                plane_flags, 0, 0, crtc_width, crtc_height, 0, 0, p_w << 16, p_h << 16);
            if (ret)
                fprintf(stderr, "failed to turn primary plane on(%s)\n", strerror(errno));
            else
                LOG_ARGS("%3d: drmModeSetPlane primary on\n", i);

            ret = drmModeSetPlane(drm.fd, overlay_plane_id, drm.crtc_id, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0);
            if (ret)
                fprintf(stderr, "failed to turn overlay plane off(%s)\n", strerror(errno));
            else
                LOG_ARGS("%3d: drmModeSetPlane overlay off\n", i);
        }

        ret = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id,
                DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
        if (ret) {
            printf("failed to queue page flip to fb (%s)\n", strerror(errno));
            return -1;
        }

        while (waiting_for_flip) {
            ret = select(drm.fd + 1, &fds, NULL, NULL, NULL);
            if (ret < 0) {
                printf("select err: %s\n", strerror(errno));
                return ret;
            } else if (ret == 0) {
                printf("select timeout!\n");
                return -1;
            } else if (FD_ISSET(0, &fds)) {
                printf("user interrupted!\n");
                break;
            }
            drmHandleEvent(drm.fd, &evctx);
        }

        if (bo)
            release_gbm_bo(gbm.surface1, bo);
        bo = bo_next;
        bo_next = NULL;

        if (bo2)
            release_gbm_bo(gbm.surface2, bo2);
        bo2 = bo2_next;
        bo2_next = NULL;
    }

    destroy_png_buffer(png_handle_primary);
    destroy_png_buffer(png_handle_secondary);

    return 0;
}
