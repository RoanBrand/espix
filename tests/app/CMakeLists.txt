# espix test app — a standalone project, NOT part of the espix firmware and NOT
# one of the examples under apps/.
#
# It lives here rather than in apps/ for two reasons: tools/build-apps.sh globs
# apps/*/ and would otherwise build it into every rootfs image, and it is not an
# example of anything -- it is a set of levers the test suite pulls.
#
# Built by `make test-app`, staged to fsroot/home/esp/testapp, and copied to the
# device by the suite when the one there is missing or stale.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(testapp)

include(elf_loader)
project_elf(testapp)
