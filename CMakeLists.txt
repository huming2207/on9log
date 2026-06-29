idf_component_register(
        SRCS "esp_stdio_log_vfs.c" "on9log.c" "on9log_esp_port.c" "on9log_port_weak.c" "on9log_esp_vfs.c" "on9log_esp_isr.c"
        INCLUDE_DIRS "."
        REQUIRES "log"
        PRIV_REQUIRES "vfs" "freertos" "esp_ringbuf"
)

if(CONFIG_ON9LOG_PLAIN_TEXT)
    # Import fmt as a third-party CMake subproject. Only the core formatting
    # library is needed by on9log; fmt's standalone targets are excluded.
    set(FMT_DOC OFF)
    set(FMT_INSTALL OFF)
    set(FMT_OS OFF)
    set(FMT_TEST OFF)
    add_subdirectory(external/fmt EXCLUDE_FROM_ALL)

    target_link_libraries(${COMPONENT_LIB} PUBLIC fmt::fmt)
endif()
