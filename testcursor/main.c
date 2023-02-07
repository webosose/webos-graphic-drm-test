/* @@@LICENSE
*
* Copyright (c) 2023 LG Electronics, Inc.
*
* Confidential computer software. Valid license from LG required for
* possession, use or copying. Consistent with FAR 12.211 and 12.212,
* Commercial Computer Software, Computer Software Documentation, and
* Technical Data for Commercial Items are licensed to the U.S. Government
* under vendor's standard commercial license.
*
* LICENSE@@@
*/

#include <time.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <sched.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>       //  For mmap()
#include <fcntl.h>
#include <string.h>
#include <mqueue.h>
#include <sys/stat.h>

#include "im_openapi.h"
#include "im_openapi_input_type.h"

#include <xf86drmMode.h>

/* TODO: remove this after libim provide this */
extern void        IM_CURSOR_DRAW_Init_GAL (int device, int crtc_id);

#define LOG(...)   do { printf(__VA_ARGS__); } while (0)
#define CHECK_ERROR(ret) \
    do { \
        if (ret != IM_OK) { \
            LOG("[%s][%d] Error ret: %d", __FUNCTION__, __LINE__, ret); \
            goto err; \
        } \
    } while (0);

static uint32_t keyEventCallback(uint32_t key, IM_KEY_COND_T keyCond, IM_ADDITIONAL_INPUT_INFO_T event)
{
    LOG("[%s][%d] key: %d",  __FUNCTION__, __LINE__, key);
    return 0;
}

static uint32_t mouseEventCallback(int posX, int posY, uint32_t keyCode, IM_KEY_COND_T keyCond,
    IM_ADDITIONAL_INPUT_INFO_T event)
{
    LOG("[%s][%d] (%d, %d) key: %d",  __FUNCTION__, __LINE__, posX, posY, keyCode);
    return 0;
}

static uint32_t touchEventCallback(IM_INPUT_TOUCH_EVENT_T **pTouchEvents)
{
    LOG("[%s][%d] %p",  __FUNCTION__, __LINE__, pTouchEvents);
    return 0;
}

static IM_INPUT_CALLBACKS_T s_inputCallback = {
    keyEventCallback,
    mouseEventCallback,
    touchEventCallback
};

/* from libdrm/tests/modetest.c  */
struct resources {
	drmModeRes *res;

	struct crtc *crtcs;
};

struct crtc {
	drmModeCrtc *crtc;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	drmModeModeInfo *mode;
};

static int get_crtc_id(int fd)
{
    int i;
    struct resources *res;
	res = calloc(1, sizeof(*res));
	if (!res)
		return -1;

    res->res = drmModeGetResources(fd);
	if (!res->res) {
		fprintf(stderr, "drmModeGetResources failed: %s\n",
			strerror(errno));
        return -1;
    }

	res->crtcs = calloc(res->res->count_crtcs, sizeof(*res->crtcs));
	if (!res->crtcs)
        return -1;


#define get_resource(_res, __res, type, Type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			(_res)->type##s[i].type =				\
				drmModeGet##Type(fd, (_res)->__res->type##s[i]); \
			if (!(_res)->type##s[i].type)				\
				fprintf(stderr, "could not get %s %i: %s\n",	\
					#type, (_res)->__res->type##s[i],	\
					strerror(errno));			\
		}								\
	} while (0)

	get_resource(res, res, crtc, Crtc);

    int crtc_id = -1;
    fprintf(stderr, "res->res->count_crtcs: %d\n", res->res->count_crtcs);
	for (i = 0; i < res->res->count_crtcs; i++) {
		struct crtc *_crtc = &res->crtcs[i];
		drmModeCrtc *crtc = _crtc->crtc;
		if (!crtc)
			continue;

        if (crtc->crtc_id != -1) {
            crtc_id = crtc->crtc_id;
            break;
        }
    }

    return crtc_id;
}

int main(int argc, char *argv[])
{
    int device_fd = -1;
    char* drm_device_path = "/dev/dri/card0";

    if (argc == 2 && !strcmp(argv[1], "--help")) {
        fprintf(stderr, "\nUsage:\n"
            "testcursor <drm device path> <crtc_id>\n"
            "           /dev/dri/card0\n");
        exit(-1);
    }
    if (argc > 1)
        drm_device_path = argv[1];
    device_fd = open(drm_device_path, O_RDWR | O_CLOEXEC);
    if (device_fd == -1) {
        fprintf(stderr, "Exit: open(%s) returns %d\n", drm_device_path, device_fd);
        exit(-1);
    }
    int crtc_id = -1;
    if (argc > 2) {
        crtc_id = atoi(argv[2]);
        fprintf(stderr, "Use CRTC id(%d) from command line\n", crtc_id);
    } else {
        crtc_id = get_crtc_id(device_fd);
        fprintf(stderr, "CRTC id from %s(%d) is %d\n", drm_device_path, device_fd, crtc_id);
    }
    if (crtc_id == -1) {
        fprintf(stderr, "Fail to get CRTC id from %d(%s) is %d\n", crtc_id, drm_device_path, device_fd);
        exit(-1);
    }

    LOG("[%s][%d] Using drm_device_path(%s)(%d) with CRTC(%d)",
        __FUNCTION__, __LINE__, drm_device_path, device_fd, crtc_id);
    IM_CURSOR_DRAW_Init_GAL(device_fd, crtc_id);

    int ret;
    ret = IM_RegisterInputRecvCallback(&s_inputCallback);
    CHECK_ERROR(ret);
    ret = IM_StartReadingInputEvent(IM_INPUT_RECV_WEBOS);
    CHECK_ERROR(ret);

    ret = IM_SetInputDispatchType(IM_INPUT_DISPATCH_ALL);
    CHECK_ERROR(ret);
    ret = IM_SetCursorVisibility(1);
    CHECK_ERROR(ret);
    ret = IM_SetCursorShape(IM_CURSOR_TYPE_A, IM_CURSOR_SIZE_M, IM_CURSOR_STATE_NORMAL);
    /* it fails sometimes  */
    /*
     * CHECK_ERROR(ret);
     */

    unsigned short int x = 0;
    unsigned short int y = 0;

    char *mode = getenv("CURSOR_AUTO");

    while (mode) {
        LOG("[%s][%d] (%d, %d)",  __FUNCTION__, __LINE__, x, y);
        ret = IM_SetCursorVisibility(1);
        CHECK_ERROR(ret);

        ret = IM_SetCursorPosition(x, y);
        CHECK_ERROR(ret);

        usleep(100000);

        x += 50;

        if (x >= 1920) {
            x = 0;
            y += 50;
        }
        if (y >= 1080) {
            break;
        }
    }

    LOG("Ctrl+C to quit");
    while (1) {
        usleep(100000);
    }

err:
    IM_StopReadingInputEvent();
    return 0;
}
