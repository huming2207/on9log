idf_component_register(
        SRCS "log_vfs_dev.c" "on9log.c"
        INCLUDE_DIRS "."
        REQUIRES "log"
        PRIV_REQUIRES "vfs" "esp_hw_support" "soc"
)
