# The partition tables the loader may write, generated from the same CSVs the
# project builds with so there is one source of truth. They differ only in
# storage's size, which is the point: the table is a size fix-up for the flash
# the board actually has.
set(espix_gen "${IDF_PATH}/components/partition_table/gen_esp32part.py")
set(espix_out "${CMAKE_CURRENT_BINARY_DIR}")
foreach(size 8mb 16mb)
    execute_process(
        COMMAND "${PYTHON}" "${espix_gen}" -q
                "${CMAKE_CURRENT_LIST_DIR}/../../partitions/esp32s3-${size}.csv"
                "${espix_out}/pt-${size}.bin"
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "loader: cannot generate the esp32s3-${size} table")
    endif()
endforeach()
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../../tools/emit-array.py"
            "${espix_out}/espix_partition_tables.h"
            espix_pt_8mb  "${espix_out}/pt-8mb.bin"
            espix_pt_16mb "${espix_out}/pt-16mb.bin"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "loader: cannot emit the partition tables")
endif()

idf_component_register(
    SRCS
        "loader_main.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES
        esp_partition
        app_update
        esp_app_format
        bootloader_support
        spi_flash
        freertos
        nvs_flash
        joltwallet__littlefs
)
