#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <drm_fourcc.h>
#include <errno.h>

#define _USE_MATH_DEFINES
#include <math.h>

#include <sys/time.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

bool verbose = false;

struct egl {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;
};

const static struct egl *egl;

struct gbm {
    struct gbm_device *dev;
    struct gbm_surface *surface;
};

static struct gbm gbm;

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

struct drm_fb {
    struct gbm_bo *bo;
    uint32_t fb_id;
    int drm_fd;
};

struct drm {
    int fd;
    drmModeModeInfo *mode;
    uint32_t crtc_id;
    uint32_t connector_id;

	/* only used for atomic: */
    uint32_t plane_id;
	struct plane *primary_plane;
	struct crtc *crtc;
	struct connector *connector;
};

static struct drm drm;

static char *default_primary_info = "31@1920x1080";
static char *default_location = "/usr/share/drmplanes";
static const int default_num_triangles = 1;
static const int default_wait_flag = 0;

#define WAIT_FLAG_BEFORE_SWAPBUFFERS 1

static int default_crtc_width = 3840;
static int default_crtc_height = 2160;

uint32_t find_crtc_for_encoder(const drmModeRes *resources,
    const drmModeEncoder *encoder) {
    int i;

    for (i = 0; i < resources->count_crtcs; i++) {
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

static int init_drm_atomic_plane(uint32_t primary_plane_id) {
    drm.primary_plane = calloc(1, sizeof(*drm.primary_plane));

#define get_plane_resource(plane, type, Type, id) do {        \
        drm.plane->type = drmModeGet##Type(drm.fd, id); \
        if (!drm.plane->type) {                         \
            printf("could not get %s %i: %s\n",         \
                #type, id, strerror(errno));            \
            return -1;                                  \
        }                                               \
    } while (0)

    get_plane_resource(primary_plane, plane, Plane, primary_plane_id);

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

    return 0;
}

bool parse_resolution(char* resolution, int *w, int *h) {
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

int init_drm(struct drm *drm, char *device_path, char *mode_str) {
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

static int init_drm_atomic(char *device_path, char *mode_str) {
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

int init_gbm(struct gbm *gbm, int fd, int p_w, int p_h, uint32_t format) {
    printf("init_gbm: primary: %dx%d", p_w, p_h);

    gbm->dev = gbm_create_device(fd);

    uint64_t modifier = DRM_FORMAT_MOD_LINEAR;

    gbm->surface = gbm_surface_create_with_modifiers(gbm->dev, p_w, p_h,
        format, &modifier, 1);
    printf("gbm->surface created by gbm_surface_create_with_modifiers\n");
    if (!gbm->surface) {
        gbm->surface = gbm_surface_create(gbm->dev, p_w, p_h, format,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!gbm->surface) {
            printf("failed to create gbm surface\n");
            return -1;
        }
    }

    return 0;
}

int match_config_to_visual(EGLDisplay egl_display,
    EGLint visual_id,
    EGLConfig *configs,
    int count) {
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

bool egl_choose_config(EGLDisplay egl_display, const EGLint *attribs,
    EGLint visual_id, EGLConfig *config_out) {
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

int init_egl(struct egl *egl, const struct gbm *gbm, uint32_t format) {
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

    egl->display = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm->dev, NULL);

    if (!eglInitialize(egl->display, &major, &minor)) {
        printf("failed to initialize\n");
        return -1;
    }

    printf("Using display %p with EGL version %d.%d\n",
        egl->display, major, minor);

    printf("EGL Version \"%s\"\n", eglQueryString(egl->display, EGL_VERSION));
    printf("EGL Vendor \"%s\"\n", eglQueryString(egl->display, EGL_VENDOR));
    printf("EGL Extensions \"%s\"\n", eglQueryString(egl->display, EGL_EXTENSIONS));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        printf("failed to bind api EGL_OPENGL_ES_API\n");
        return -1;
    }

    if (!egl_choose_config(egl->display, config_attribs, format, &egl->config)) {
        printf("failed to choose config\n");
        return -1;
    }

    egl->context = eglCreateContext(egl->display, egl->config, EGL_NO_CONTEXT, context_attribs);
    if (egl->context == NULL) {
        printf("failed to create context 1\n");
        return -1;
    }

    egl->surface = eglCreateWindowSurface(egl->display, egl->config, gbm->surface, NULL);
    if (egl->surface == EGL_NO_SURFACE) {
        printf("failed to create egl surface 1\n");
        return -1;
    }

    /* connect the context to the surface */
    eglMakeCurrent(egl->display, egl->surface, egl->surface, egl->context);

    printf("GL Extensions: \"%s\"\n", glGetString(GL_EXTENSIONS));

    return 0;
}

static void print_usage(const char *progname) {
    printf("Usage:\n");
    printf("    %s -p <plane_id>@<width>x<height> -v -D <device_path> -m <mode_str> -w <glFinish_flag> -t <num_triangles>\n", progname);
    printf("\n");
    printf("    -p primary plane info (default: %s)\n", default_primary_info);
    printf("    -c CRTC resolution to be used drmModeSetPlane for primary plane (default: %dx%d)\n",
        default_crtc_width, default_crtc_height);
    printf("    -v verbose\n");
    printf("    -w glFinish flag, 0: no glFinish, 1: add glFinish before eglSwapBuffers (default: %d)\n", default_wait_flag);
    printf("    -t number of triangles for rendering (default: %d)\n", default_num_triangles);
    printf("    -D drm device path (default: /dev/dri/card0)\n");
    printf("    -m mode preferred (default: NULL, mode with highest resolution)\n");
    printf("    -f FOURCC format (default: AR24)\n");
    printf("    -l resource location (default: /usr/share/drmplanes)\n");
    printf("    -h help\n");
    printf("\n");
    printf("Example:\n");
    printf("    %s -p 31@3840x2160 -v -m 3840x2160 -f AR24 -c 3840x2160 -w 0 -t 1\n", progname);
}

static int add_connector_property(drmModeAtomicReq *req, uint32_t obj_id,
                    const char *name, uint64_t value) {
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
                const char *name, uint64_t value) {
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
    const char *name, uint64_t value) {
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
    uint32_t crtc_x, uint32_t crtc_y) {
    add_plane_property(req, plane_id, "FB_ID", fb_id);
    add_plane_property(req, plane_id, "CRTC_ID", crtc_id);
    add_plane_property(req, plane_id, "SRC_X", 0);
    add_plane_property(req, plane_id, "SRC_Y", 0);
    add_plane_property(req, plane_id, "SRC_W", src_width << 16);
    add_plane_property(req, plane_id, "SRC_H", src_height << 16);
    add_plane_property(req, plane_id, "CRTC_X", crtc_x);
    add_plane_property(req, plane_id, "CRTC_Y", crtc_y);
    add_plane_property(req, plane_id, "CRTC_W", crtc_width);
    add_plane_property(req, plane_id, "CRTC_H", crtc_height);
}

static int drm_atomic_mode_set(drmModeAtomicReq *req, uint32_t flags) {
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

void drm_fb_destroy_callback(struct gbm_bo *bo, void *data) {
    struct drm_fb *fb = data;
    struct gbm_device *gbm = gbm_bo_get_device(bo);

    if (fb->fb_id) {
        drmModeRmFB(fb->drm_fd, fb->fb_id);
        printf("drmModeRmFB fb_id: %d\n", fb->fb_id);
    }

    free(fb);
}

struct drm_fb * drm_fb_get_from_bo(int fd, struct gbm_bo *bo) {
    struct drm_fb *fb = gbm_bo_get_user_data(bo);
    uint32_t width, height, stride, handle, format;
    int ret;

    if (fb)
        return fb;

    fb = calloc(1, sizeof (*fb));
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
    printf("drmModeAddFB2(%d, %d) fb_id: %d\n", width, height, fb->fb_id);

    if (ret) {
        printf("failed to create fb: %s\n", strerror(errno));
        free(fb);
        return NULL;
    }

    gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

    return fb;
}

bool parse_plane(char* resolution, int *id, int *w, int *h) {
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

bool lock_new_surface(int fd, const struct gbm *gbm, struct gbm_surface *gbm_surface, struct gbm_bo **out_bo, struct drm_fb **out_fb) {
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm_surface);
    if (!bo) {
        fprintf(stderr, "fail to lock front buffer(%s)\n", strerror(errno));
        return false;
    }

    *out_bo = bo;
    struct drm_fb *fb = drm_fb_get_from_bo(fd, bo);
    if (!fb) {
        fprintf(stderr, "fail to get fb from bo(%s)\n", strerror(errno));
        return false;
    }
    *out_fb = fb;
    return true;
}

static const char glVertexShader_triangle[] =
	"uniform mat4 rotation;\n"
	"attribute vec4 pos;\n"
	"attribute vec4 color;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"  gl_Position = rotation * vec4(pos.xyz, 1.0);\n"
	"  v_color = color;\n"
	"}\n";

static const char *glFragmentShader_triangle =
	"precision mediump float;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"  gl_FragColor = v_color;\n"
	"}\n";

static const GLfloat vertices_triangle[3][2] = {
    { -0.5, -0.5 },
    {  0.5, -0.5 },
    {  0,    0.5 }
};
static const GLfloat colors_triangle[3][3] = {
    { 1, 0, 0 },
    { 0, 1, 0 },
    { 0, 0, 1 }
};

GLuint program_triangle;
GLuint loc_pos_triangle, loc_col_triangle;
GLuint vRotation_triangle;

GLuint loadShader(GLenum shaderType, const char* shaderSource) {
    GLuint shader = glCreateShader(shaderType);
    if (shader)
    {
        glShaderSource(shader, 1, &shaderSource, NULL);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled)
        {
            GLint infoLen = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
            if (infoLen)
            {
                char * buf = (char*) malloc(infoLen);
                if (buf)
                {
                    glGetShaderInfoLog(shader, infoLen, NULL, buf);
                    fprintf(stderr, "Could not Compile Shader %d:\n%s\n", shaderType, buf);
                    free(buf);
                }
                glDeleteShader(shader);
                shader = 0;
            }
        }
    }
    return shader;
}

GLuint createProgram(const char* vertexSource, const char * fragmentSource) {
    GLuint vertexShader = loadShader(GL_VERTEX_SHADER, vertexSource);
    if (!vertexShader) {
        return 0;
    }
    GLuint fragmentShader = loadShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!fragmentShader) {
        return 0;
    }
    GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program , vertexShader);
        glAttachShader(program, fragmentShader);
        glLinkProgram(program);
        GLint linkStatus = GL_FALSE;
        glGetProgramiv(program , GL_LINK_STATUS, &linkStatus);
        if( linkStatus != GL_TRUE) {
            GLint bufLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
            if (bufLength) {
                char* buf = (char*) malloc(bufLength);
                if (buf) {
                    glGetProgramInfoLog(program, bufLength, NULL, buf);
                    fprintf(stderr, "Could not link program:\n%s\n", buf);
                    free(buf);
                }
            }
            glDeleteProgram(program);
            program = 0;
        }
    }
    return program;
}

int init_render(int p_w, int p_h) {
    // program triangle
    program_triangle = createProgram(glVertexShader_triangle, glFragmentShader_triangle);
    if (!program_triangle) {
        fprintf(stderr, "Could not create program (triangle)\n");
        return -1;
    }

    glUseProgram(program_triangle);

	loc_pos_triangle = 0;
	loc_col_triangle = 1;

	glBindAttribLocation(program_triangle, loc_pos_triangle, "pos");
	glBindAttribLocation(program_triangle, loc_col_triangle, "color");

	vRotation_triangle = glGetUniformLocation(program_triangle, "rotation");

    glViewport(0, 0, p_w, p_h);

    glUseProgram(0);
    return 0;
}

int draw_render_triangle(int i, float trans_x, float trans_y) {
    glUseProgram(program_triangle);

    GLfloat rotation[4][4] = {
		{ 1, 0, 0, 0 },
		{ 0, 1, 0, 0 },
		{ 0, 0, 1, 0 },
		{ 0, 0, 0, 1 }
	};

    GLfloat angle = (i % 360) * M_PI / 180.0;
	rotation[0][0] =  cos(angle);
	rotation[0][2] =  sin(angle);
	rotation[2][0] = -sin(angle);
	rotation[2][2] =  cos(angle);

    // Since OpenGL is column major,
    rotation[3][0] += trans_x;
    rotation[3][1] += trans_y;

    glUniformMatrix4fv(vRotation_triangle, 1, GL_FALSE, (GLfloat *) rotation);

    glVertexAttribPointer(loc_pos_triangle, 2, GL_FLOAT, GL_FALSE, 0, vertices_triangle);
	glVertexAttribPointer(loc_col_triangle, 3, GL_FLOAT, GL_FALSE, 0, colors_triangle);
	glEnableVertexAttribArray(loc_pos_triangle);
	glEnableVertexAttribArray(loc_col_triangle);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glDisableVertexAttribArray(loc_pos_triangle);
	glDisableVertexAttribArray(loc_col_triangle);

    glUseProgram(0);

    return 0;
}

const struct egl* init_egl_loader(int drm_fd, const struct gbm *gbm, uint32_t format) {
    int ret;

    static struct egl eglc;
    memset(&eglc, 0x0, sizeof(eglc));

    ret = init_egl(&eglc, gbm, format);
    if (ret)
        return NULL;

    return &eglc;
}

#define MAX_LINUX_INPUT_DEVICES 16
struct epoll_event g_events[MAX_LINUX_INPUT_DEVICES];

static void pageFlipHandler(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, void *user_data) {
    static long long int flip_recent_time = 0;
    long long int flip_current_time;
    flip_current_time = (tv_sec) * 1000000 +  tv_usec;

    if(flip_recent_time != 0) {
        printf("\nThe interval of calling drmHandleEvent %lld ms\n\n", (flip_current_time - flip_recent_time)/1000);
    }
    else {
        printf("\ninitial calling drmHandleEvent\n\n");
    }
    flip_recent_time = flip_current_time;
}

bool waitForDrm(int epoll_fd) {
    int nfds = epoll_wait(epoll_fd, g_events, MAX_LINUX_INPUT_DEVICES, -1);
    if(nfds == 0) return false;
    if(nfds < 0) {
        fprintf(stderr, "ERROR[epoll_wait]: ndfs has a negative value\n");
        return false;
    }
    for(int i = 0; i < nfds; i++) {
        if(g_events[i].data.fd == drm.fd) {
            printf("drm event\n");

            drmEventContext drmEvent;
            memset(&drmEvent, 0, sizeof(drmEvent));
            drmEvent.version = 2;
            drmEvent.vblank_handler = NULL;
            drmEvent.page_flip_handler = pageFlipHandler;
            drmHandleEvent(drm.fd, &drmEvent);
        }
    }
    return true;
}

void test_draw_triangles(int frame_idx, int n){
    glUseProgram(program_triangle);

    glClearColor(0.0, 0.0, 0.0, 0.5);
    glClear(GL_COLOR_BUFFER_BIT);

    if(n == 1) {
        draw_render_triangle(frame_idx, 0.0f, 0.0f);
    }
    else if(n == 2) {
        draw_render_triangle(frame_idx, -0.5f, 0.0f);
        draw_render_triangle(frame_idx + 1, 0.5f, 0.0f);
    }
    else if(n == 3) {
        draw_render_triangle(frame_idx, -0.5f, -0.5f);
        draw_render_triangle(frame_idx + 1, 0.5f, -0.5f);
        draw_render_triangle(frame_idx + 2, -0.5f, 0.5f);
    }
    else if(n > 3){
        int grid_len_x = sqrt(n);
        int grid_len_y = grid_len_x;
        if(grid_len_x * grid_len_x >= n){
            grid_len_x--;
            grid_len_y--;
        }
        else if(grid_len_x * (grid_len_x + 1) >= n){
            grid_len_y--;
        }

        float distance_x = 1.0f / grid_len_x;
        float distance_y = 1.0f / grid_len_y;

        int count = 0;
        for(int count = 0, idx_x = 0, idx_y = 0; count < n; count++) {
            draw_render_triangle(frame_idx + count, -0.5f + idx_x * distance_x, -0.5f + idx_y * distance_y);
            idx_x++;
            if(idx_x > grid_len_x) {
                idx_x = 0;
                idx_y++;
            }
            if(idx_y > grid_len_y) {
                break;
            }
        }
    }

    glUseProgram(0);
}

int main(int argc, char *argv[]) {
    struct gbm_bo *bo_curr = NULL, *bo_next = NULL;
    struct drm_fb *fb = NULL;
    drmModeAtomicReq *req_curr = NULL, *req_prev = NULL;
    uint32_t frame_idx = 0;
    int ret;

    int opt;
    int wait_flag = default_wait_flag;
    int primary_plane_id = 31;
    char *primary_plane_info = default_primary_info;
    char *device_path = "/dev/dri/card0";
    char *mode_str = NULL;
    char *crtc_str = NULL;
    uint32_t format = GBM_FORMAT_ARGB8888;
    char *location = default_location;

    int num_triangles = default_num_triangles;

    while ((opt = getopt(argc, argv, "hvap:D:m:f:l:c:t:w:")) != -1) {
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
            case 'v':
                verbose = true;
                break;
            case 'p':
                primary_plane_info = optarg;
                break;
            case 'D':
                device_path = optarg;
                break;
            case 't':
                num_triangles = strtoul(optarg, NULL, 10);
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
            case 'w':
                wait_flag = strtoul(optarg, NULL, 10);
                break;
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

    if (!parse_plane(primary_plane_info, &primary_plane_id, &p_w, &p_h)) {
        printf("failed to parse primary resolution %s\n", primary_plane_info);
        return ret;
    }

    ret = init_drm_atomic(device_path, mode_str);
    if (ret) {
        printf("failed to initialize DRM\n");
        return ret;
    }

    printf("drm->mode: %dx%d\n", drm.mode->hdisplay, drm.mode->vdisplay);

    ret = init_drm_atomic_plane(primary_plane_id);
    if (ret) {
        printf("failed to initialize atomic planes\n");
        return ret;
    }

    ret = init_gbm(&gbm, drm.fd, p_w, p_h, format);
    if (ret) {
        printf("failed to initialize GBM\n");
        return ret;
    }

    egl = init_egl_loader(drm.fd, &gbm, format);
    if (!egl) {
        printf("failed to initialize EGL\n");
        return -1;
    }

    uint32_t plane_flags = 0;

    int crtc_width = default_crtc_width;
    int crtc_height = default_crtc_height;

    if (crtc_str) {
        if (!parse_resolution(crtc_str, &crtc_width, &crtc_height)) {
            fprintf(stderr, "failed to parse crtc_str: %s\n", crtc_str);
            return -1;
        }
    }
    printf("CRTC width: %d height: %d\n", crtc_width, crtc_height);


    int epoll_fd = epoll_create(MAX_LINUX_INPUT_DEVICES);
    if(epoll_fd < 0) {
        fprintf(stderr, "ERROR[epoll_create]: epoll_fd has a negative value\n");
        return -1;
    }

    struct epoll_event drmfd_event;
    drmfd_event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    drmfd_event.data.fd = drm.fd;

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, drm.fd, &drmfd_event) < 0) {
        fprintf(stderr, "ERROR[epoll_ctl]: failed to control\n");
        return -1;
    }

    if(init_render(p_w, p_h)){
        return -1;
    }

    uint32_t flags = (DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET);

    while (true) {
        frame_idx++;

        eglMakeCurrent(egl->display, egl->surface, egl->surface, egl->context);

        if(bo_next) {
            GLenum err;
            while((err = glGetError()) != GL_NO_ERROR) {
                printf("GL ERROR: %d\n", err);
            }

            test_draw_triangles(frame_idx, num_triangles);

            while((err = glGetError()) != GL_NO_ERROR) {
                printf("GL ERROR: %d\n", err);
            }

            waitForDrm(epoll_fd);

            if(bo_curr) {
                gbm_surface_release_buffer(gbm.surface, bo_curr);
            }
            bo_curr = bo_next;
            bo_next = NULL;

            if(req_prev) {
                drmModeAtomicFree(req_prev);
                req_prev = NULL;
            }
        }

        if(wait_flag & WAIT_FLAG_BEFORE_SWAPBUFFERS) {
            glFinish();
        }

        eglSwapBuffers(egl->display, egl->surface);

        if (!lock_new_surface(drm.fd, &gbm, gbm.surface, &bo_next, &fb)) {
            fprintf(stderr, "fail to add surface 1\n");
            return 1;
        }

        if(!req_curr) {
            req_curr = drmModeAtomicAlloc();

            if(!req_curr) {
                fprintf(stderr, "ERROR[drmModeAtomicAlloc]: failed to alloc\n");
                continue;
            }

            drm_atomic_mode_set(req_curr, flags);

            drm_atomic_set_plane_properties(req_curr, primary_plane_id, drm.crtc_id, fb->fb_id,
                p_w, p_h, crtc_width, crtc_height, 0, 0);
        }

        ret = drmModeAtomicCommit(drm.fd, req_curr, flags, NULL);
        printf("%i: drmModeAtomicCommit(%d %p %x) returns %d(%s)\n", frame_idx, drm.fd, req_curr, flags, ret, strerror(ret));

        if(ret) {
            fprintf(stderr, "ERROR[drmModeAtomicCommit]: failed to commit\n");
            continue;
        }

        req_prev = req_curr;
        req_curr = NULL;
    }

    return 0;
}
