# The shared cases in tests/ run here and in the Linux unit test.
idf_component_register(
    SRCS
        "on9log_qemu_test.cpp"
        "../../../tests/on9log_test.cpp"
        "../../../tests/on9log_test_cases.cpp"
        "../../../tests/on9log_test_c_calls.c"
        "../../../tests/on9log_test_tu_a.cpp"
        "../../../tests/on9log_test_tu_b.cpp"
    INCLUDE_DIRS "../../../tests"
    PRIV_REQUIRES on9log esp_app_format esp_timer freertos vfs
)
