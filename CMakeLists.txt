cmake_minimum_required(VERSION 3.20)

if(COMMAND idf_component_register)
    idf_component_register(
        SRCS
            "esp_stdio_log_vfs.c"
            "on9log.c"
            "on9log_esp_port.c"
            "on9log_port_weak.c"
            "on9log_esp_vfs.c"
            "on9log_esp_isr.c"
        INCLUDE_DIRS "."
        REQUIRES "log"
        PRIV_REQUIRES "vfs" "freertos" "esp_ringbuf"
    )

    if(CONFIG_ON9LOG_PLAIN_TEXT)
        # Only plain-text C++ logging needs fmt. Keep its standalone targets
        # out of the ESP-IDF component build.
        set(FMT_DOC OFF)
        set(FMT_INSTALL OFF)
        set(FMT_OS OFF)
        set(FMT_TEST OFF)
        add_subdirectory(external/fmt EXCLUDE_FROM_ALL)
        target_link_libraries(${COMPONENT_LIB} PUBLIC fmt::fmt)
    endif()
else()
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        project(on9log LANGUAGES C CXX)
        set(_on9log_build_tests_default ON)
        set(_on9log_build_demo_default ON)
    else()
        set(_on9log_build_tests_default OFF)
        set(_on9log_build_demo_default OFF)
    endif()

    if(NOT UNIX)
        message(FATAL_ERROR "The standalone on9log shim supports Linux and macOS only")
    endif()

    option(ON9LOG_PLAIN_TEXT "Emit formatted plain text instead of binary packets" OFF)
    option(ON9LOG_BUILD_TESTS "Build the Unix host smoke test" ${_on9log_build_tests_default})
    option(ON9LOG_BUILD_DEMO "Build the comprehensive Unix host demo" ${_on9log_build_demo_default})

    find_package(Threads REQUIRED)

    add_library(on9log
        on9log.c
        on9log_unix_port.c
        on9log_unix_stdio.c
    )
    add_library(on9log::on9log ALIAS on9log)

    target_include_directories(on9log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(on9log PUBLIC c_std_11 cxx_std_23)
    target_compile_definitions(on9log PUBLIC
        ON9LOG_PLAIN_TEXT=$<BOOL:${ON9LOG_PLAIN_TEXT}>
    )
    target_link_libraries(on9log PUBLIC Threads::Threads)

    if(ON9LOG_PLAIN_TEXT)
        set(FMT_DOC OFF)
        set(FMT_INSTALL OFF)
        set(FMT_OS OFF)
        set(FMT_TEST OFF)
        add_subdirectory(external/fmt EXCLUDE_FROM_ALL)
        target_link_libraries(on9log PUBLIC fmt::fmt)
    endif()

    if(ON9LOG_BUILD_TESTS)
        enable_testing()
        add_executable(on9log_unix_smoke tests/on9log_unix_smoke.cpp)
        target_link_libraries(on9log_unix_smoke PRIVATE on9log::on9log)
        add_test(NAME on9log_unix_smoke COMMAND on9log_unix_smoke)

        add_executable(on9log_core_regression tests/on9log_core_regression.cpp)
        target_link_libraries(on9log_core_regression PRIVATE on9log::on9log)
        add_test(NAME on9log_core_regression COMMAND on9log_core_regression)
    endif()

    if(ON9LOG_BUILD_DEMO)
        add_executable(on9log_unix_demo examples/on9log_unix_demo.cpp)
        target_link_libraries(on9log_unix_demo PRIVATE on9log::on9log)
        if(NOT ON9LOG_PLAIN_TEXT)
            # Binary packets carry 32-bit format/tag addresses. Linux host IDs
            # resolve directly against a non-PIE ELF. macOS
            # requires PIE and the stdio shim emits its Mach-O image slide as
            # stream metadata instead.
            if(NOT APPLE)
                target_link_options(on9log_unix_demo PRIVATE -no-pie)
            endif()
        endif()
        if(ON9LOG_BUILD_TESTS)
            add_test(NAME on9log_unix_demo_run COMMAND on9log_unix_demo)
        endif()
    endif()

    unset(_on9log_build_tests_default)
    unset(_on9log_build_demo_default)
endif()
