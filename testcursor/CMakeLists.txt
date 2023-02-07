# @@@LICENSE
#
# Copyright (c) 2023 LG Electronics, Inc.
#
# LICENSE@@@

include(FindPkgConfig)

# TODO: remove this if libim.pc provided
set(NO_LIBIM_PC true)

pkg_check_modules(GBM REQUIRED gbm IMPORTED_TARGET)
pkg_check_modules(DRM REQUIRED libdrm IMPORTED_TARGET)
pkg_check_modules(GLESv2 REQUIRED glesv2 IMPORTED_TARGET)
pkg_check_modules(EGL REQUIRED egl IMPORTED_TARGET)
pkg_check_modules(GLIB2 REQUIRED glib-2.0 IMPORTED_TARGET)

add_executable(testcursor main.c)
target_link_libraries(testcursor PUBLIC
    PkgConfig::GBM
    PkgConfig::DRM
    PkgConfig::GLESv2
    PkgConfig::EGL
    PkgConfig::GLIB2
)

if (NO_LIBIM_PC)
    include_directories($ENV{PKG_CONFIG_SYSROOT_DIR}/usr/include/im)
    target_link_libraries(testcursor PUBLIC -lim)
else()
    pkg_check_modules(IM REQUIRED libim IMPORTED_TARGET)
    target_link_libraries(testcursor PUBLIC PkgConfig::IM)
endif()

target_compile_options(testcursor PRIVATE -Werror)

install(TARGETS testcursor DESTINATION ${WEBOS_INSTALL_BINDIR})
