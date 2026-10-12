#!/usr/bin/env bash
# Runs on9log's tests. CI calls this; it works the same on a developer machine.
#
#   ci/run_tests.sh host   Linux: GCC and Clang, binary and plain text, ASan/UBSan, and
#                          TSan where address randomisation can be turned off
#   ci/run_tests.sh host-reflection
#                          Linux with ON9LOG_CXX_REFLECTION (needs GCC 16+ as ON9LOG_CI_GCC/GXX)
#   ci/run_tests.sh qemu   ESP32-S3 in QEMU, binary and plain text (needs ESP-IDF exported
#                          and qemu-xtensa installed: idf_tools.py install qemu-xtensa)
#   ci/run_tests.sh qemu-reflection
#                          ESP32-S3 in QEMU with CONFIG_ON9LOG_CXX_REFLECTION (needs an
#                          ESP-IDF whose toolchain is GCC 16+, e.g. master, exported)
#   ci/run_tests.sh        host and qemu
#
# Compilers can be chosen with ON9LOG_CI_GCC/GXX/CLANG/CLANGXX (default gcc, g++,
# clang, clang++). Build trees go to build-ci/ unless ON9LOG_CI_DIR says otherwise.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${ON9LOG_CI_DIR:-$root/build-ci}"
gcc="${ON9LOG_CI_GCC:-gcc}"
gxx="${ON9LOG_CI_GXX:-g++}"
clang="${ON9LOG_CI_CLANG:-clang}"
clangxx="${ON9LOG_CI_CLANGXX:-clang++}"

warnings="-Wall -Wextra -Werror"
# Plain-text builds still compile some binary-only helpers that nothing calls.
plain_warnings="$warnings -Wno-unused-function -Wno-unused-variable"
sanitizers="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"

# host_build NAME CC CXX FLAGS [CMAKE ARGS...]: configure and build one build tree.
host_build() {
    local name=$1 cc=$2 cxx=$3 flags=$4
    shift 4
    echo "=== host: $name"
    cmake -S "$root" -B "$out/host-$name" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
        -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" "$@"
    cmake --build "$out/host-$name" -j
}

# host_test NAME CC CXX FLAGS [CMAKE ARGS...]: build, then run its tests.
host_test() {
    host_build "$@"
    ctest --test-dir "$out/host-$1" --output-on-failure
}

run_host() {
    host_test gcc-binary "$gcc" "$gxx" "$warnings"
    host_test gcc-plain-text "$gcc" "$gxx" "$plain_warnings" -DON9LOG_PLAIN_TEXT=ON
    host_test clang-binary "$clang" "$clangxx" "$warnings"
    host_test clang-plain-text "$clang" "$clangxx" "$plain_warnings" -DON9LOG_PLAIN_TEXT=ON
    host_test asan-binary "$gcc" "$gxx" "$sanitizers"
    host_test asan-plain-text "$gcc" "$gxx" "$sanitizers" -DON9LOG_PLAIN_TEXT=ON
    run_tsan
}

# TSan aborts at random ("unexpected memory mapping") when the kernel randomises mmap
# addresses as widely as recent kernels do, so its tests run with randomisation off.
# Docker's default seccomp profile forbids that, so there TSan is skipped, loudly.
run_tsan() {
    local no_aslr=(setarch "$(uname -m)" -R)
    if ! "${no_aslr[@]}" true 2>/dev/null; then
        echo "=== host: tsan SKIPPED: cannot turn off address randomisation here (container?)"
        return 0
    fi
    host_build tsan "$gcc" "$gxx" "-fsanitize=thread"
    "${no_aslr[@]}" ctest --test-dir "$out/host-tsan" --output-on-failure
}

# C++26 reflection: plain Logger literals go to .noload. The build fails without GCC 16+.
run_host_reflection() {
    host_test gcc-reflection "$gcc" "$gxx" "$warnings" -DON9LOG_CXX_REFLECTION=ON
    host_test asan-reflection "$gcc" "$gxx" "$sanitizers" -DON9LOG_CXX_REFLECTION=ON
}

# qemu_test CONFIG: build test_apps/qemu with sdkconfig.ci.CONFIG and run it in QEMU.
qemu_test() {
    local config=$1
    local app="$root/test_apps/qemu"
    local build="$out/qemu-$config"
    local idf=(idf.py -C "$app" -B "$build" -DSDKCONFIG="$build/sdkconfig"
               "-DSDKCONFIG_DEFAULTS=$app/sdkconfig.defaults;$app/sdkconfig.ci.$config")
    echo "=== qemu: $config"
    "${idf[@]}" build

    # The app restarts when it is done; -no-reboot turns that into QEMU exiting. A run
    # takes seconds, so the timeout only ends a hang.
    timeout 300 "${idf[@]}" qemu --qemu-extra-args="-no-reboot" </dev/null | tee "$build/qemu.log"
    if ! grep -q "ON9T RESULT: PASS" "$build/qemu.log"; then
        echo "qemu-$config: tests failed, see $build/qemu.log"
        return 1
    fi

    # The image ID the firmware reports must be the SHA-256 of the ELF a host would decode with.
    local reported elf
    reported=$(sed -n 's/.*ON9T ELF SHA256: \([0-9a-f]*\).*/\1/p' "$build/qemu.log")
    elf=$(sha256sum "$build/on9log_qemu_test.elf" | cut -d' ' -f1)
    if [ "$reported" != "$elf" ]; then
        echo "qemu-$config: firmware image ID '$reported' is not the ELF SHA-256 $elf"
        return 1
    fi

    # A plain C++ Logger literal is in the flashed image without reflection, and only in
    # the ELF (.noload) with it. The binary configs log one to check this both ways.
    if [ "$config" != plain_text ]; then
        local literal="on9t plain literal" in_bin=no expected=yes
        grep -qaF "$literal" "$build/on9log_qemu_test.bin" && in_bin=yes
        [ "$config" = reflection ] && expected=no
        if [ "$in_bin" != "$expected" ] || ! grep -qaF "$literal" "$build/on9log_qemu_test.elf"; then
            echo "qemu-$config: '$literal' in flash: $in_bin, expected $expected (and always in the ELF)"
            return 1
        fi
    fi
}

run_qemu() {
    qemu_test binary
    qemu_test plain_text
}

case "${1:-all}" in
host) run_host ;;
host-reflection) run_host_reflection ;;
qemu) run_qemu ;;
qemu-reflection) qemu_test reflection ;;
all)
    run_host
    run_qemu
    ;;
*)
    echo "usage: $0 [host|host-reflection|qemu|qemu-reflection|all]" >&2
    exit 2
    ;;
esac
echo "=== all tests passed"
