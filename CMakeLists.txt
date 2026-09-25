# espix — a Unix(-like) kernel/runtime for ESP32, on top of ESP-IDF.
#
# Components under components/ are auto-discovered by the IDF build system,
# so no EXTRA_COMPONENT_DIRS is needed here.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espix)

# Make off_t 64 bits, for every translation unit in the firmware. Before the
# patch hooks below only in the sense of order in this file; what matters is that
# it is after project(), because that is when IDF's build properties exist.
#
# An ABI decision rather than a preference, so it is on for every app as well --
# see cmake/offt64.cmake, which each app's CMakeLists.txt includes too.
set(ESPIX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/offt64.cmake")

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
# any patched file changes — which is what a reinstall or an upgrade does to
# them, and what `git checkout` does in the IDF tree. Without that, a reverted
# IDF would sail past the hook and fail as an undefined reference to
# esp_vfs_fat_ctx_create() pointing at espix rather than at the real cause: that
# is not a hypothetical, it is what the first version of this hook did.
#
# Every file the script writes is listed, not just the first two: ffconf.h and
# the two diskio files carry the exFAT and 64-bit work, and leaving them out
# would let a `git checkout` of those revert the patch with nothing to notice --
# the same failure, wearing a different coat.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${IDF_PATH}/components/fatfs/vfs/vfs_fat.c"
    "${IDF_PATH}/components/fatfs/vfs/vfs_fat_internal.h"
    "${IDF_PATH}/components/fatfs/src/ffconf.h"
    "${IDF_PATH}/components/fatfs/diskio/diskio_impl.h"
    "${IDF_PATH}/components/fatfs/diskio/diskio_bdl.c"
    "${IDF_PATH}/components/fatfs/diskio/diskio_sdmmc.c"
    "${IDF_PATH}/components/fatfs/diskio/diskio_wl.c"
    "${IDF_PATH}/components/fatfs/diskio/diskio_rawflash.c")
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch-fatfs.py"
            --idf-path "${IDF_PATH}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE espix_fatfs_patch_result)
if(NOT espix_fatfs_patch_result EQUAL 0)
    message(FATAL_ERROR "tools/patch-fatfs.py failed; see the message above")
endif()

# Keep _lseek_r's 32-bit ABI, now that off_t is 64 bits. It is an alias for
# esp_vfs_lseek and so follows the type, but its callers are prebuilt and do not:
# newlib's stdio.o carries the seek logic fseek() ends up in and was compiled with
# a 32-bit off_t, and the ROM's libc stub table declares an int offset for the same
# reason. Without this, `fseek` seeks somewhere else -- measured, as an SFTP upload
# failing with "seek failed" -- so the boundary keeps its width and the widening
# happens inside it. espix's own lseek()/pread()/pwrite() stay 64-bit.
#
# Three files, the third outside IDF: the declaration in the toolchain's reent.h is
# written in terms of _off_t, which is the type being widened, so it has to be
# pinned to the width the libc was built with -- for the same reason as the
# definition, and harmlessly for a project that never widens anything.
#
# See tools/patch-libc-offt.py and docs/UPSTREAM.md.
#
# Re-configure when any patched file changes, as for fatfs above: a reinstall or
# a `git checkout` in the IDF tree reverts them, and reverting one without noticing
# is a build that fails for a reason that points at espix.
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-sysroot
    OUTPUT_VARIABLE espix_toolchain_sysroot
    OUTPUT_STRIP_TRAILING_WHITESPACE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${IDF_PATH}/components/vfs/vfs_calls.c"
    "${IDF_PATH}/components/esp_libc/src/reent_syscalls.c"
    "${espix_toolchain_sysroot}/include/reent.h")
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch-libc-offt.py"
            --idf-path "${IDF_PATH}"
            --toolchain-root "${espix_toolchain_sysroot}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE espix_libc_offt_patch_result)
if(NOT espix_libc_offt_patch_result EQUAL 0)
    message(FATAL_ERROR "tools/patch-libc-offt.py failed; see the message above")
endif()

# Add READ CAPACITY(16), READ(16) and WRITE(16) to espressif/usb_host_msc,
# which speaks only the (10) commands and their 32-bit LBAs -- unreadable past
# 2TiB, and READ CAPACITY(10) itself saturates there rather than reporting the
# real size. Found on a real 3.6TB Samsung T9, which read back as exactly
# 2,199,023,255,040 bytes. See docs/USB-HOST.md and tools/patch-msc.py.
#
# After project(), same reasoning as the littlefs hook above: the component
# manager has to have downloaded the tree first.
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch-msc.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE espix_msc_patch_result)
if(NOT espix_msc_patch_result EQUAL 0)
    message(FATAL_ERROR "tools/patch-msc.py failed; see the message above")
endif()

# Teach the lwext4 core the component manager downloads to honour the metadata
# checksum seed, without which it refuses any ext4 volume made by e2fsprogs 1.47
# or later -- which is every volume a current Linux creates. See
# docs/KNOWN-ISSUES.md and tools/lwext4-csum-seed.patch.
#
# After project(), same reasoning as the two hooks above: the download happens
# when the component manager runs, which is inside project(). The patch lands in
# the core rather than in the port, because that is where the checksum seeding
# is -- the component carries it as a submodule, so this is the submodule's tree.
#
# Temporary by construction: tools/lwext4-csum-seed.patch is the same change
# ready to send to gkostka/lwext4, and this hook goes when it lands.
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch-lwext4.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE espix_lwext4_patch_result)
if(NOT espix_lwext4_patch_result EQUAL 0)
    message(FATAL_ERROR "tools/patch-lwext4.py failed; see the message above")
endif()

# Teach GMF to allocate from PSRAM before internal RAM. Its object allocator
# prefers internal, and on the S31 a full GMF pipeline there starves Bluedroid's
# BTU task, which aborts in fixed_queue_new the moment a sink connects.
# See tools/patch-gmf.py, and docs/AUDIO.md.
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch-gmf.py"
            --components-path "${CMAKE_CURRENT_SOURCE_DIR}/managed_components"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE espix_gmf_patch_result)
if(NOT espix_gmf_patch_result EQUAL 0)
    message(FATAL_ERROR "tools/patch-gmf.py failed; see the message above")
endif()

# fsroot/ is the tree the dev rootfs is packaged from. It is gitignored and a
# fresh clone has none, so create it: the device builds its own filesystem on
# first boot, and an empty one is a fine starting point.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/fsroot")

# The image itself is made by tools/make-fs-image.sh rather than during every
# `idf.py build`: it is sized to its contents and grown to the whole partition
# by the kernel's grow_on_mount, so there is no reason to write a partition-sized
# file into the build directory on every build. `make flash-fs` builds and
# flashes it; release.sh builds the apps-only variant a release ships.

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
    # ALL, because nothing packages an image at build time any more; the dev
    # rootfs is built on demand by `make flash-fs`.
    add_custom_target(espix_apps ALL
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-apps.sh"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building apps/ and staging into fsroot/bin"
        VERBATIM
        USES_TERMINAL)
endif()

# A symbol an app resolves a layer below is usable only if its definition is in
# the image -- and naming it to *prove* that is what pulls it in and grows the
# kernel, so the check runs the other way round, on the linked ELF, where it costs
# the image nothing. It catches elf_loader's symbol tables being switched off, or
# a newer elf_loader dropping a name; an app finds either as "undefined symbol",
# at load time, on a device.
#
# See tools/check-abi.py, and the list it guards in components/espix_proc/abi_libc.c.
#
# After the link, so this is POST_BUILD rather than configure-time like the patch
# hooks above: there is no ELF to read until then.
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check-abi.py"
            --elf "$<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>"
            --nm "${CMAKE_NM}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Checking the layer-below symbols are in the image"
    VERBATIM)

