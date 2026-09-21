# espix loader -- the small app that makes one big kernel slot work.
#
# A standalone IDF project on purpose: it must stay small and must not inherit
# the kernel's dependency graph. It needs littlefs to read the kernel files,
# app_update to write the slot, and NVS for the small amount of state that says
# which file is which. No networking, no TLS, no espix components.
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espix_loader)

#
# LTO was tried here and does not link: IDF's assembly stubs reference symbols
# that the LTO pass removes (xt_unhandled_exception). Not worth chasing for a
# loader that is already 153 KiB. See docs/OTA.md section 11.
#
