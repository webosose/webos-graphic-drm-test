#include "drm-common.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <drm_fourcc.h>
#include <assert.h>

uint32_t find_crtc_for_encoder(const drmModeRes *resources,
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

uint32_t find_crtc_for_connector(int fd, const drmModeRes *resources,
    const drmModeConnector *connector) {
    int i;

    for (i = 0; i < connector->count_encoders; i++) {
        const uint32_t encoder_id = connector->encoders[i];
        drmModeEncoder *encoder = drmModeGetEncoder(fd, encoder_id);

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

int init_drm(struct drm *drm, char *device_path, char *mode_str)
{
    drmModeRes *resources;
    drmModeConnector *connector = NULL;
    drmModeEncoder *encoder = NULL;
    int i, area;

    drm->fd = open(device_path, O_RDWR);

    if (drm->fd < 0) {
        printf("could not open drm device\n");
        return -1;
    }

    resources = drmModeGetResources(drm->fd);
    if (!resources) {
        printf("drmModeGetResources failed: %s\n", strerror(errno));
        return -1;
    }

    /* find a connected connector: */
    for (i = 0; i < resources->count_connectors; i++) {
        connector = drmModeGetConnector(drm->fd, resources->connectors[i]);
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
            drm->mode = current_mode;
        }

        int current_area = current_mode->hdisplay * current_mode->vdisplay;
        if (current_area > area) {
            drm->mode = current_mode;
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
                drm->mode = current_mode;
                break;
            }
        }
    }

    if (!drm->mode) {
        printf("could not find mode!\n");
        return -1;
    }

    /* find encoder: */
    for (i = 0; i < resources->count_encoders; i++) {
        encoder = drmModeGetEncoder(drm->fd, resources->encoders[i]);
        if (encoder->encoder_id == connector->encoder_id)
            break;
        drmModeFreeEncoder(encoder);
        encoder = NULL;
    }

    if (encoder) {
        drm->crtc_id = encoder->crtc_id;
    } else {
        uint32_t crtc_id = find_crtc_for_connector(drm->fd, resources, connector);
        if (crtc_id == 0) {
            printf("no crtc found!\n");
            return -1;
        }

        drm->crtc_id = crtc_id;
    }

    drm->connector_id = connector->connector_id;

    return 0;
}

bool parse_resolution(char* resolution, int *w, int *h)
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

int init_gbm(struct gbm *gbm, int fd, int p_w, int p_h, int o_w, int o_h, uint32_t format)
{
    printf("init_gbm: primary: %dx%d overlay: %dx%d\n", p_w, p_h, o_w, o_h);

    gbm->dev = gbm_create_device(fd);

    uint64_t modifier = DRM_FORMAT_MOD_LINEAR;

    gbm->surface1 = gbm_surface_create_with_modifiers(gbm->dev, p_w, p_h,
        format, &modifier, 1);
    LOG("gbm->surface1 created by gbm_surface_create_with_modifiers\n");
    if (!gbm->surface1) {
        gbm->surface1 = gbm_surface_create(gbm->dev, p_w, p_h, format,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!gbm->surface1) {
            printf("failed to create gbm surface1\n");
            return -1;
        }
    }

    gbm->surface2 = gbm_surface_create_with_modifiers(gbm->dev, o_w, o_h,
        format, &modifier, 1);
    LOG("gbm->surface2 created by gbm_surface_create_with_modifiers\n");
    if (!gbm->surface2) {
        gbm->surface2 = gbm_surface_create(gbm->dev, o_w, o_h, format,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!gbm->surface2) {
            printf("failed to create gbm surface2\n");
            return -1;
        }
    }
    return 0;
}

extern bool verbose;

void log_message_with_args(const char *msg, ...) {
    if (!verbose)
        return;

    va_list args;
    va_start(args, msg);
    vprintf(msg, args);
    va_end(args);
}

int
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

bool
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

int init_gl(struct gl *gl, struct gbm *gbm, uint32_t format)
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

    gl->display = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm->dev, NULL);

    if (!eglInitialize(gl->display, &major, &minor)) {
        printf("failed to initialize\n");
        return -1;
    }

    printf("Using display %p with EGL version %d.%d\n",
        gl->display, major, minor);

    printf("EGL Version \"%s\"\n", eglQueryString(gl->display, EGL_VERSION));
    printf("EGL Vendor \"%s\"\n", eglQueryString(gl->display, EGL_VENDOR));
    printf("EGL Extensions \"%s\"\n", eglQueryString(gl->display, EGL_EXTENSIONS));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        printf("failed to bind api EGL_OPENGL_ES_API\n");
        return -1;
    }

    if (!egl_choose_config(gl->display, config_attribs, format,
            &gl->config)) {
        printf("failed to choose config\n");
        return -1;
    }

    gl->context = eglCreateContext(gl->display, gl->config,
        EGL_NO_CONTEXT, context_attribs);
    if (gl->context == NULL) {
        printf("failed to create context 1\n");
        return -1;
    }

    gl->surface1 = eglCreateWindowSurface(gl->display, gl->config, gbm->surface1, NULL);
    if (gl->surface1 == EGL_NO_SURFACE) {
        printf("failed to create egl surface 1\n");
        return -1;
    }

    gl->surface2 = eglCreateWindowSurface(gl->display, gl->config, gbm->surface2, NULL);
    if (gl->surface2 == EGL_NO_SURFACE) {
        printf("failed to create egl surface 2\n");
        return -1;
    }

    /* connect the context to the surface */
    eglMakeCurrent(gl->display, gl->surface1, gl->surface1, gl->context);

    printf("GL Extensions: \"%s\"\n", glGetString(GL_EXTENSIONS));

    return 0;
}

void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
    struct drm_fb *fb = data;
    struct gbm_device *gbm = gbm_bo_get_device(bo);

    if (fb->fb_id) {
        drmModeRmFB(fb->drm_fd, fb->fb_id);
        LOG_ARGS("drmModeRmFB fb_id: %d\n", fb->fb_id);
    }

    free(fb);
}

struct drm_fb * drm_fb_get_from_bo(int fd, struct gbm_bo *bo)
{
    struct drm_fb *fb = gbm_bo_get_user_data(bo);
    uint32_t width, height, stride, handle, format;
    int ret;

    if (fb)
        return fb;

    fb = calloc(1, sizeof *fb);
    fb->bo = bo;
    fb->drm_fd = fd;

    width = gbm_bo_get_width(bo);
    height = gbm_bo_get_height(bo);
    stride = gbm_bo_get_stride(bo);
    handle = gbm_bo_get_handle(bo).u32;
    format = gbm_bo_get_format(bo);

    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 };
    uint32_t strides[4] = { gbm_bo_get_stride(bo) };
    uint32_t offsets[4] = { 0 };

    ret = drmModeAddFB2(fd, width, height, format,
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

bool parse_plane(char* resolution, int *id, int *w, int *h)
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

bool lock_new_surface(int fd, struct gbm *gbm, struct gbm_surface *gbm_surface, struct gbm_bo **out_bo, struct drm_fb **out_fb)
{
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm_surface);
    if (!bo) {
        fprintf(stderr, "fail to lock front buffer(%s)\n", strerror(errno));
        return false;
    }
    LOG_ARGS("gbm_surface_lock_front_buffer: %s %p\n", gbm_surface_name(gbm, gbm_surface), bo);

    *out_bo = bo;
    struct drm_fb *fb = drm_fb_get_from_bo(fd, bo);
    if (!fb) {
        fprintf(stderr, "fail to get fb from bo(%s)\n", strerror(errno));
        return false;
    }
    *out_fb = fb;
    return true;
}

char* gbm_surface_name(struct gbm *gbm, struct gbm_surface *surface)
{
    if (surface == gbm->surface1)
        return "primary";
    else if (surface == gbm->surface2)
        return "overlay";
    else
        return "unknown";
}

void release_gbm_bo(struct gbm *gbm, struct gbm_surface* surface, struct gbm_bo *bo)
{
    if (bo) {
        LOG_ARGS("gbm_surface_release_buffer: %s %p\n", gbm_surface_name(gbm, surface), bo);
        gbm_surface_release_buffer(surface, bo);
    }
}

bool read_png_from_file(const char* filename, png_buffer_handle *out_png_buffer_handle)
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

bool fill_gbm_buffer(struct gbm_bo *bo, png_buffer_handle png_buffer_handle)
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

bool read_png_and_write_to_bo(const char* filename, struct gbm_bo *bo)
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

bool add_surface(int fd, struct gl *gl, struct gbm *gbm, EGLSurface gl_surface,
    struct gbm_surface *gbm_surface, struct glcolor *color,
    struct gbm_bo **out_bo, struct drm_fb **out_fb)
{
    eglMakeCurrent(gl->display, gl_surface, gl_surface, gl->context);
    /*
     * draw_gl(0, color, gl_surface);
     */
    eglSwapBuffers(gl->display, gl_surface);
    return lock_new_surface(fd, gbm, gbm_surface, out_bo, out_fb);
}

void get_resource_path(char* fullpath, const char *location, const char *filename)
{
    fullpath[0] = '\0';
    strcat(fullpath, location);
    strcat(fullpath, "/");
    strcat(fullpath, filename);
}
