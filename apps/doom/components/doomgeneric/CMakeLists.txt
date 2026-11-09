# The doomgeneric engine, built from the fetched (not vendored) source; the
# list and the settings are apps/doom/sources.cmake, which the U-mode build
# shares.
include("${CMAKE_CURRENT_LIST_DIR}/../../sources.cmake")

idf_component_register(SRCS ${DG_SRCS}
                       INCLUDE_DIRS ${DG_INC})

# PUBLIC: the platform layer includes doomgeneric.h and must agree on pixel_t.
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${DG_DEFS})
target_compile_options(${COMPONENT_LIB} PRIVATE ${DG_OPTS})
