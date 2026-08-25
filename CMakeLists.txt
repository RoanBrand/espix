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

# Build apps/ and stage the ELFs into fsroot/bin before that image is packaged.
#
# Apps are separate IDF projects rather than components, so `idf.py build` does
# not otherwise produce them and a fresh clone would flash a rootfs with an
# empty /bin. tools/build-apps.sh skips any app whose staged binary is already
# newer than its sources, so the usual cost is a few seconds of idf.py startup.
#
# Turn it off while iterating on the firmware alone:
#   idf.py -DESPIX_BUILD_APPS=OFF build
option(ESPIX_BUILD_APPS "Build apps/ and stage them into fsroot/bin" ON)

if(ESPIX_BUILD_APPS)
    add_custom_target(espix_apps
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-apps.sh"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building apps/ and staging into fsroot/bin"
        VERBATIM
        USES_TERMINAL)

    # The image is packaged from fsroot/, so staging has to have happened first.
    add_dependencies(littlefs_storage_bin espix_apps)
endif()
