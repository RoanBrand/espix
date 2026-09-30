# The platform layer, as a component rather than a second object of main.
#
# project_elf() links `libmain.a` first, then ELF_COMPONENTS in order. main
# references the engine, so main.o is pulled; nothing in main references the
# seam, so a doom_espix.o left in libmain.a would never be scanned -- and
# because the output is a shared object, the leftover DG_* would not even be a
# link error. It would fail on the device, at load time. Listing the platform
# after the engine is what puts its symbols in front of the engine's
# references.
idf_component_register(SRCS "doom_espix.c"
                       INCLUDE_DIRS
                           "."
                           "${CMAKE_SOURCE_DIR}/../../components/espix_proc/include"
                           "${CMAKE_SOURCE_DIR}/../../components/espix_display/include"
                       REQUIRES doomgeneric esp_timer)
