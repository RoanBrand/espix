# espix — a Unix(-like) kernel/runtime for ESP32, on top of ESP-IDF.
#
# Components under components/ are auto-discovered by the IDF build system,
# so no EXTRA_COMPONENT_DIRS is needed here.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espix)

# Build fsroot/ into a LittleFS image for the "storage" partition. This is how
# the initial rootfs (and, until SCP exists, cross-compiled apps) get onto the
# device: `idf.py storage-flash`.
#
# Deliberately NOT FLASH_IN_PROJECT: that would rewrite the whole rootfs on
# every `idf.py flash`, destroying anything created on the device. For a system
# whose point is a persistent, mutable filesystem, flashing it has to be an
# explicit act.
#
# Provided by managed_components/joltwallet__littlefs/project_include.cmake.
littlefs_create_partition_image(storage fsroot)
