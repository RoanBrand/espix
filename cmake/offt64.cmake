# Make off_t 64 bits. Included by the firmware's top-level CMakeLists.txt and by
# every app's, because it is an ABI: an app that disagrees with the kernel about
# the width of off_t disagrees about every struct stat they pass each other.
#
# The header explains why it is a force-include rather than a patch, and what it
# costs. See ../cmake/offt64.h and docs/ROADMAP.md.
#
# After project(), always: idf_build_set_property() is what IDF's own build
# system uses for this (tools/cmake/build.cmake), and it reaches every component.
#
# C and C++ only, not COMPILE_OPTIONS. The blanket property reaches the assembler
# as well, and a header force-included into a .S file is a header the assembler
# reads -- which is what the first version did, and it failed on every `typedef`
# in machine/_default_types.h. off_t64.h is guarded against that too, so a .S file
# that happens to be preprocessed as C still comes out unharmed.

idf_build_set_property(C_COMPILE_OPTIONS
    "-include${ESPIX_ROOT}/cmake/offt64.h" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS
    "-include${ESPIX_ROOT}/cmake/offt64.h" APPEND)
