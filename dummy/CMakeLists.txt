# @@@LICENSE
#
# Copyright (c) 2023 LG Electronics, Inc.
#
# LICENSE@@@

project(dummy)
cmake_minimum_required(VERSION 3.6)

add_executable(dummy main.c)
install(TARGETS dummy DESTINATION ${WEBOS_INSTALL_BINDIR})
