# espix — a Unix(-like) kernel/runtime for ESP32, on top of ESP-IDF.
#
# Components under components/ are auto-discovered by the IDF build system,
# so no EXTRA_COMPONENT_DIRS is needed here.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espix)

# Give joltwallet/littlefs a public custom-attribute API, which is where espix
# keeps a file's mode. The port uses LittleFS user attributes itself (mtime) but
# exposes neither them nor the `lfs_t *` they need, and ESP-IDF's VFS has no
# chmod hook to route around it — see docs/UPSTREAM.md.
#
# After project(), because that is when the component manager downloads; before
# any compiling, because ninja reads the file later. The script is idempotent
# and fails the build loudly if the version or the anchors moved.
#
# Temporary by construction: tools/esp_littlefs-attrs.patch is the same change
# ready to send upstream, and this hook goes when it lands.
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch-littlefs.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE espix_littlefs_patch_result)
if(NOT espix_littlefs_patch_result EQUAL 0)
    message(FATAL_ERROR "tools/patch-littlefs.py failed; see the message above")
endif()

# Expose an ESP-IDF FatFs entry point that mounts without registering a base
# path. Every esp_vfs_fat_*_mount() ends in esp_vfs_register_fs(), and the ops
# tables are file-scope static, so FatFs cannot otherwise be mounted *under*
# espix's VFS — which is where the permission check lives, and the reason
# components/espix_fs/vfs.c holds an ops table rather than a base path.
#
# fatfs is a built-in IDF component and the registry publishes no espressif/fatfs
# that could override it, so this patches the IDF installation rather than
# managed_components. The change is additive: esp_vfs_fat_register() still does
# exactly what it did.
#
# Temporary by construction: tools/esp_vfs_fat-ctx.patch is the same change ready
# to send upstream, and this hook goes when it lands.
#
# The script runs on every *configure*, and CMake is told to re-configure when
# either patched file changes — which is what a reinstall or an upgrade does to
# them, and what `git checkout` does in the IDF tree. Without that, a reverted
# IDF would sail past the hook and fail as an undefined reference to
# esp_vfs_fat_ctx_create() pointing at espix rather than at the real cause: that
# is not a hypothetical, it is what the first version of this hook did.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${IDF_PATH}/components/fatfs/vfs/vfs_fat.c"
    "${IDF_PATH}/components/fatfs/vfs/vfs_fat_internal.h")
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch-fatfs.py"
            --idf-path "${IDF_PATH}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE espix_fatfs_patch_result)
if(NOT espix_fatfs_patch_result EQUAL 0)
    message(FATAL_ERROR "tools/patch-fatfs.py failed; see the message above")
endif()

# Build fsroot/ into a LittleFS image for the "storage" partition. This is how
# the initial rootfs (and, until SCP exists, cross-compiled apps) get onto the
# device: `idf.py storage-flash`.
#
# Deliberately NOT FLASH_IN_PROJECT: that would rewrite the whole rootfs on
# every `idf.py flash`, destroying anything created on the device. For a system
# whose point is a persistent, mutable filesystem, flashing it has to be an
# explicit act.
#
# fsroot/ is entirely gitignored -- see the note there -- so a fresh clone has
# no such directory and the image rule below would fail on it. Create it here
# rather than committing a placeholder: the device builds its own filesystem on
# first boot, so an empty image is a perfectly good starting point, and
# ESPIX_BUILD_APPS=OFF means nothing else would create it either.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/fsroot")

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
