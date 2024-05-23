# @@@LICENSE
#
# Copyright (c) 2023 LG Electronics, Inc.
#
# LICENSE@@@

project(drmplanes)
cmake_minimum_required(VERSION 3.6)

include(webOS/webOS)
webos_modules_init(1 0 0)
include(FindPkgConfig)

pkg_check_modules(GBM REQUIRED gbm IMPORTED_TARGET)
pkg_check_modules(DRM REQUIRED libdrm IMPORTED_TARGET)
pkg_check_modules(GLESv2 REQUIRED glesv2 IMPORTED_TARGET)
pkg_check_modules(EGL REQUIRED egl IMPORTED_TARGET)
pkg_search_module(PNG REQUIRED libpng12 libpng IMPORTED_TARGET)

add_executable(drmplanes main.c readpng.c drm-common.c)
target_link_libraries(drmplanes PUBLIC
    PkgConfig::GBM
    PkgConfig::DRM
    PkgConfig::GLESv2
    PkgConfig::EGL
    PkgConfig::PNG
)

target_compile_options(drmplanes PRIVATE -Werror)

add_executable(drmplanes-atomic main-atomic.c readpng.c drm-common.c cube-smooth.c esTransform.c png-image.c)
target_link_libraries(drmplanes-atomic PUBLIC
    PkgConfig::GBM
    PkgConfig::DRM
    PkgConfig::GLESv2
    PkgConfig::EGL
    PkgConfig::PNG
    m                           # needed by esTransfrom
)

target_compile_options(drmplanes-atomic PRIVATE -Werror)

add_executable(drm-gldraw-atomic drm-gldraw-atomic.c)
target_link_libraries(drm-gldraw-atomic PUBLIC
    PkgConfig::GBM
    PkgConfig::DRM
    PkgConfig::GLESv2
    PkgConfig::EGL
    m
)

target_compile_options(drm-gldraw-atomic PRIVATE -Werror)

install(TARGETS drmplanes DESTINATION ${WEBOS_INSTALL_BINDIR})
install(TARGETS drmplanes-atomic DESTINATION ${WEBOS_INSTALL_BINDIR})
install(TARGETS drm-gldraw-atomic DESTINATION ${WEBOS_INSTALL_BINDIR})
install(FILES primary_1920x1080.png secondary_512x2160.png
    DESTINATION ${WEBOS_INSTALL_DATADIR}/drmplanes
)
