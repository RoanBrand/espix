# Make off_t 64 bits. Included by the firmware's top-level CMakeLists.txt and by
# every app's, because it is an ABI: an app that disagrees with the kernel about
# the width of off_t passes 32-bit offsets to a 64-bit lseek.
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

# Except esp_libc, which is the glue to the prebuilt picolibc and so keeps its
# 32-bit off_t: its lseek is the one the prebuilt fdopen/fseek call by name, and
# the seek callbacks it hands that library are declared with libc's own width.
# -D is processed before -include, so the header sees this and stays empty.
idf_build_get_property(espix_build_components BUILD_COMPONENTS)
if("esp_libc" IN_LIST espix_build_components)
    idf_component_get_property(espix_libc_lib esp_libc COMPONENT_LIB)
    target_compile_definitions(${espix_libc_lib} PRIVATE ESPIX_LIBC_OWN_OFF_T)
endif()
