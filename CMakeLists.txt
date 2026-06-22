idf_component_register(
        SRCS "esp_stdio_log_vfs.c" "on9log.c" "on9log_esp_vfs.c"
        INCLUDE_DIRS "."
        REQUIRES "log"
        PRIV_REQUIRES "vfs" "esp_hw_support" "soc"
)
