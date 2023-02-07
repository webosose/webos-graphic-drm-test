# @@@LICENSE
#
# Copyright (c) 2023 LG Electronics, Inc.
#
# LICENSE@@@

project(drmplanes)
cmake_minimum_required(VERSION 3.6)

include(FindPkgConfig)

pkg_check_modules(GBM REQUIRED gbm IMPORTED_TARGET)
pkg_check_modules(DRM REQUIRED libdrm IMPORTED_TARGET)
pkg_check_modules(GLESv2 REQUIRED glesv2 IMPORTED_TARGET)
pkg_check_modules(EGL REQUIRED egl IMPORTED_TARGET)

add_executable(drmplanes main.c)
target_link_libraries(drmplanes PUBLIC
    PkgConfig::GBM
    PkgConfig::DRM
    PkgConfig::GLESv2
    PkgConfig::EGL
)

target_compile_options(drmplanes PRIVATE -Werror)

install(TARGETS drmplanes DESTINATION ${WEBOS_INSTALL_BINDIR})
