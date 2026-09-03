# espix signal test app — a standalone project, NOT part of the espix firmware.
#
# Exercises the signal surface espix publishes: signal(), getpid(), an
# interruptible sleep(), and the delivery point a loop that never blocks needs.
# Written as ordinary POSIX C, which is the point — nothing here is espix-shaped
# except the one call that has no POSIX equivalent.
#
# Build:
#   idf.py -G 'Unix Makefiles' set-target esp32s3    # enables `idf.py elf`
#   idf.py elf
#
# The firmware build does this automatically and stages the result into
# fsroot/bin/sigtest; see tools/build-apps.sh.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sigtest)

include(elf_loader)
project_elf(sigtest)
