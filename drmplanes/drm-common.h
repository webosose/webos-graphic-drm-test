#ifndef DRM_COMMON_H
#define DRM_COMMON_H

#include <stdlib.h>
#include <stdbool.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "readpng.h"

#define LOG(msg)                                                        \
    log_message_with_args("%s:%d: " msg, __PRETTY_FUNCTION__, __LINE__)

#define LOG_ARGS(msg, ...)                                              \
    log_message_with_args("%s:%d: " msg, __PRETTY_FUNCTION__, __LINE__, __VA_ARGS__)

struct drm {
    int fd;
    drmModeModeInfo *mode;
    uint32_t crtc_id;
    uint32_t connector_id;

	/* only used for atomic: */
    uint32_t plane_id;
	struct plane *primary_plane;
	struct plane *overlay_plane;
	struct crtc *crtc;
	struct connector *connector;
};

struct gbm {
    struct gbm_device *dev;
    struct gbm_surface *surface1;
    struct gbm_surface *surface2;
};

struct gl {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface1;
    EGLSurface surface2;
};

struct drm_fb {
    struct gbm_bo *bo;
    uint32_t fb_id;
    int drm_fd;
};

struct glcolor {
    GLclampf r;
    GLclampf g;
    GLclampf b;
    GLclampf a;
};

uint32_t find_crtc_for_encoder(const drmModeRes *resources, const drmModeEncoder *encoder);
uint32_t find_crtc_for_connector(int fd, const drmModeRes *resources, const drmModeConnector *connector);
int init_drm(struct drm *drm, char *device_path, char *mode_str);
bool parse_resolution(char* resolution, int *w, int *h);
int init_gbm(struct gbm *gbm, int fd, int p_w, int p_h, int o_w, int o_h, uint32_t format);

void log_message_with_args(const char *msg, ...);
int match_config_to_visual(EGLDisplay egl_display, EGLint visual_id, EGLConfig *configs, int count);
bool egl_choose_config(EGLDisplay egl_display, const EGLint *attribs, EGLint visual_id, EGLConfig *config_out);
int init_gl(struct gl *gl, struct gbm *gbm, uint32_t format);

void drm_fb_destroy_callback(struct gbm_bo *bo, void *data);
struct drm_fb * drm_fb_get_from_bo(int fd, struct gbm_bo *bo);
bool parse_plane(char* resolution, int *id, int *w, int *h);
bool lock_new_surface(int fd, struct gbm *gbm, struct gbm_surface *gbm_surface, struct gbm_bo **out_bo, struct drm_fb **out_fb);
char* gbm_surface_name(struct gbm *gbm, struct gbm_surface *surface);
void release_gbm_bo(struct gbm *gbm, struct gbm_surface* surface, struct gbm_bo *bo);
bool add_surface(int fd, struct gl *gl, struct gbm *gbm, EGLSurface gl_surface,
    struct gbm_surface *gbm_surface, struct glcolor *color,
    struct gbm_bo **out_bo, struct drm_fb **out_fb);

bool read_png_from_file(const char* filename, png_buffer_handle *out_png_buffer_handle);
bool fill_gbm_buffer(struct gbm_bo *bo, png_buffer_handle png_buffer_handle);
bool read_png_and_write_to_bo(const char* filename, struct gbm_bo *bo);

void get_resource_path(char* fullpath, const char *location, const char *filename);

#endif /* DRM_COMMON_H */
