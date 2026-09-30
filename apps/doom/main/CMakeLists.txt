idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS
                           "."
                           "${CMAKE_SOURCE_DIR}/../../components/espix_proc/include"
                           "${CMAKE_SOURCE_DIR}/../../components/espix_display/include"
                       REQUIRES doomgeneric esp_timer)
