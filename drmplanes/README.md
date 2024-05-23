# drm-gldraw-atomic

'drm-gldraw-atomic' is an application with DRM, EGL.

This application draws triangles for testing rendering result.

When the rendering overloaded with drawing many triangles, some devices show rendering result with tearing.

## commands

```
Usage:
    drm-gldraw-atomic -p <plane_id>@<width>x<height> -v -D <device_path> -m <mode_str> -w <glFinish_flag> -t <num_triangles>

    -p primary plane info (default: 31@1920x1080)
    -c CRTC resolution to be used drmModeSetPlane for primary plane (default: 3840x2160)
    -v verbose
    -w glFinish flag, 0: no glFinish, 1: add glFinish before eglSwapBuffers (default: 0)
    -t number of triangles for rendering (default: 1)
    -D drm device path (default: /dev/dri/card0)
    -m mode preferred (default: NULL, mode with highest resolution)
    -f FOURCC format (default: AR24)
    -l resource location (default: /usr/share/drmplanes)
    -h help
```

- glFinish Flag
    - 0: glFinish is not called. (default)
    - 1: glFinish is called before drmModeAtomicCommit.
- number of triangles
    - The number of triangles rendered for testing tearing with rendering overload.
    - The default value '1' means that a triangle is rendered.

```
example

Full HD
drm-gldraw-atomic -p 31@1920x1080 -v -m 1920x1080 -c 1920x1080 -w 0 -t 1

4K Resolution
drm-gldraw-atomic -p 31@3840x1440 -v -m 3840x1440 -c 3840x1440 -w 0 -t 1

Tearing test without glFinish (draw 400 triangles)
drm-gldraw-atomic -p 31@1920x1080 -v -m 1920x1080 -c 1920x1080 -w 0 -t 400

Tearing test with glFinish (draw 400 triangles)
drm-gldraw-atomic -p 31@1920x1080 -v -m 1920x1080 -c 1920x1080 -w 1 -t 400
```
