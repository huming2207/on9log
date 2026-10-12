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

# QEMU only: ESP-IDF 6.2-dev's USB-Serial-JTAG driver (linked by vfs) turns the BBPLL on
# at boot and waits for a calibration that QEMU never finishes. Skip that wait here;
# see __wrap_rtc_clk_bbpll_add_consumer in on9log_qemu_test.cpp.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=rtc_clk_bbpll_add_consumer")
