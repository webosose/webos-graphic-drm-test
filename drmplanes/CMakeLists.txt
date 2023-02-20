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

add_executable(drmplanes main.c readpng.c)
target_link_libraries(drmplanes PUBLIC
    PkgConfig::GBM
    PkgConfig::DRM
    PkgConfig::GLESv2
    PkgConfig::EGL
    PkgConfig::PNG
)

target_compile_options(drmplanes PRIVATE -Werror)

install(TARGETS drmplanes DESTINATION ${WEBOS_INSTALL_BINDIR})
install(FILES primary_1920x1080.png secondary_512x2160.png
    DESTINATION ${WEBOS_INSTALL_DATADIR}/drmplanes
)
