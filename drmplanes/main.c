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
static struct drm drm;
static struct egl egl;

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

static char* surface_name(EGLSurface surface)
{
    if (surface == egl.surface1)
        return "primary";
    else if (surface == egl.surface2)
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

static void page_flip_handler(int fd, unsigned int frame,
    unsigned int sec, unsigned int usec, void *data)
{
    int *waiting_for_flip = data;
    *waiting_for_flip = 0;
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

    ret = init_drm(&drm, device_path, mode_str);
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

    ret = init_gbm(&gbm, drm.fd, p_w, p_h, o_w, o_h, format);
    if (ret) {
        printf("failed to initialize GBM\n");
        return ret;
    }

    ret = init_egl(&egl, &gbm, format);
    if (ret) {
        printf("failed to initialize EGL\n");
        return ret;
    }

    uint32_t plane_flags = 0;

    /* surface1 */
    if (!lock_new_surface(drm.fd, &gbm, gbm.surface1, &bo, &fb)) {
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
                if (!lock_new_surface(drm.fd, &gbm, gbm.surface2, &bo2, &fb2)) {
                    fprintf(stderr, "fail to add surface 2\n");
                    return 1;
                }
                if (!fill_gbm_buffer(bo2, png_handle_secondary)) {
                    fprintf(stderr, "fail to fill secondary bo\n");
                    return 1;
                }
            } else {
                eglMakeCurrent(egl.display, egl.surface2, egl.surface2, egl.context);
                /*
                 * draw_gl(i, &blue, egl.surface2);
                 */
                eglSwapBuffers(egl.display, egl.surface2);

                if (!lock_new_surface(drm.fd, &gbm, gbm.surface2, &bo2_next, &fb2)) {
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
                eglMakeCurrent(egl.display, egl.surface1, egl.surface1, egl.context);
                draw_gl(i, &black, egl.surface1);
                eglSwapBuffers(egl.display, egl.surface1);
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
            eglMakeCurrent(egl.display, egl.surface1, egl.surface1, egl.context);
            /*
             * draw_gl(i, &red, egl.surface1);
             */
            eglSwapBuffers(egl.display, egl.surface1);

            if (!lock_new_surface(drm.fd, &gbm, gbm.surface1, &bo_next, &fb)) {
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
