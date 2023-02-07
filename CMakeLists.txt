# @@@LICENSE
#
# Copyright (c) 2023 LG Electronics, Inc.
#
# LICENSE@@@

cmake_minimum_required(VERSION 3.6)

# This is to avoid error in do_install when there is nothing to install
add_subdirectory(dummy)
if (TESTCURSOR_ENABLED)
    include(CheckSymbolExists)
    list(APPEND CMAKE_REQUIRED_LIBRARIES im)
    check_symbol_exists(IM_CURSOR_DRAW_Init_GAL im/im_openapi_cursor.h HAVE_DRM_CURSOR)

    if (HAVE_DRM_CURSOR)
        add_subdirectory(testcursor)
    endif()
endif()

if (DRMPLANES_ENABLED)
    add_subdirectory(drmplanes)
endif()

