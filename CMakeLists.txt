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
        PRIV_REQUIRES "vfs" "freertos" "esp_ringbuf" "esp_app_format" "esp_timer"
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

    if(CONFIG_ON9LOG_CXX_REFLECTION)
        # C++ code including on9log.hpp needs C++26 reflection. A toolchain
        # without it fails here or at on9log.hpp's #error, never silently.
        target_compile_options(${COMPONENT_LIB} PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-freflection>)
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
    option(ON9LOG_CXX_REFLECTION "Move plain C++ format literals to .noload with C++26 reflection" OFF)
    if(ON9LOG_CXX_REFLECTION AND ON9LOG_PLAIN_TEXT)
        message(FATAL_ERROR "ON9LOG_CXX_REFLECTION requires ON9LOG_PLAIN_TEXT=OFF")
    endif()
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
        ON9LOG_CXX_REFLECTION=$<BOOL:${ON9LOG_CXX_REFLECTION}>
    )
    if(ON9LOG_CXX_REFLECTION)
        target_compile_features(on9log PUBLIC cxx_std_26)
        target_compile_options(on9log PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-freflection>)
    endif()
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
        add_executable(on9log_unix_smoke tests/on9log_unix_smoke.cpp tests/on9log_test.cpp)
        target_link_libraries(on9log_unix_smoke PRIVATE on9log::on9log)
        add_test(NAME on9log_unix_smoke COMMAND on9log_unix_smoke)

        # The same cases also run on ESP32 in QEMU (test_apps/qemu).
        add_executable(on9log_unit
            tests/on9log_unit_linux.cpp
            tests/on9log_test.cpp
            tests/on9log_test_cases.cpp
            tests/on9log_test_c_calls.c
        )
        target_link_libraries(on9log_unit PRIVATE on9log::on9log)
        if(NOT APPLE)
            # Like the demo: without PIE, literal addresses fit the 32-bit wire
            # IDs, so the test covers both by-address and inline strings. The
            # build-id is the image ID the BOOT packet reports.
            target_link_options(on9log_unit PRIVATE -no-pie -Wl,--build-id)
        endif()
        add_test(NAME on9log_unit COMMAND on9log_unit)

        # Builds that must fail: the test passes when the compiler prints the expected error.
        function(on9log_add_compile_fail_test name expected_error)
            add_test(NAME on9log_compile_fail_${name}
                     COMMAND ${CMAKE_CXX_COMPILER} -std=gnu++23 -fsyntax-only
                             -I${CMAKE_CURRENT_SOURCE_DIR} ${ARGN}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/on9log_compile_fail.cpp)
            set_tests_properties(on9log_compile_fail_${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected_error}")
        endfunction()
        if(NOT ON9LOG_PLAIN_TEXT)
            on9log_add_compile_fail_test(too_many_args "at most 32 encoded arguments" -DON9T_TOO_MANY_ARGS)
            on9log_add_compile_fail_test(isr_string_arg "does not support dynamic string arguments" -DON9T_ISR_STRING_ARG)
            on9log_add_compile_fail_test(unsupported_type "unsupported on9log C" -DON9T_UNSUPPORTED_TYPE)
            on9log_add_compile_fail_test(chunk_too_large "is too large for a packet header"
                                         -DON9T_CONFIG_ONLY -DON9LOG_BUFFER_CHUNK_SIZE=65523)
            on9log_add_compile_fail_test(isr_packet_too_small "must be at least 32"
                                         -DON9T_CONFIG_ONLY -DON9LOG_ISR_PACKET_MAX=31)
            on9log_add_compile_fail_test(reflection_with_plain_text "needs binary mode"
                                         -DON9T_CONFIG_ONLY -DON9LOG_CXX_REFLECTION=1 -DON9LOG_PLAIN_TEXT=1
                                         -I${CMAKE_CURRENT_SOURCE_DIR}/external/fmt/include)
            # Compiled as C++23, so no compiler has reflection here: the option must stop the build.
            on9log_add_compile_fail_test(reflection_unsupported "lacks C\\+\\+26 reflection"
                                         -DON9T_CONFIG_ONLY -DON9LOG_CXX_REFLECTION=1)
        endif()
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
