# on9log

Custom binary and plain-text logging library for embedded platforms, with a
Linux/macOS shim for host execution and testing.

The host-side decoder library and CLI tools are at: https://github.com/huming2207/on9log_host

## Overview

`on9log` provides a compact binary packet format for embedded logging, optimized for:
- fast in-firmware emission with minimal CPU overhead;
- transport-agnostic forwarding (UART, MQTT, WebSocket etc.);
- small on-flash footprint via ELF-only format strings.

The firmware sends only addresses for format and tag strings. The matching host decoder (separate repo) resolves them from the firmware ELF, parses printf- or `{}`-style format strings, and renders colorized human-readable output.

## Architecture

```
┌─────────────────────────────────┐
│  Firmware (C + C++)             │
│                                 │
│  on9log.h / on9log.c            │  Core packet producer
│  on9log.hpp                     │  C++20 header-only wrapper
│  on9log_esp_vfs.c               │  SLIP+CRC UART transport
│  on9log_esp_isr.c               │  ISR-safe ringbuffer path
│  esp_stdio_log_vfs.c            │  Shared stdio VFS framer
└──────────────┬──────────────────┘
               │  typed SLIP frames over UART
               ▼
       Host decoder & CLI
     (separate repository)
```

On Linux and macOS, `on9log_unix_port.c` supplies pthread locking and a
monotonic clock, while `on9log_unix_stdio.c` supplies the default host sink.
Binary packets are written to stdio using the same typed SLIP/CRC envelope as
the ESP transport, so packet boundaries survive redirection and pipes. Both
transports enforce the shared 3072-byte `ON9LOG_TRANSPORT_MAX_PAYLOAD` cap and
drop an oversized binary packet as a whole. Plain text is written directly to
stdio.
On macOS binary builds, the sink writes a leading image-slide metadata line so
the host decoder can resolve 32-bit IDs against the ASLR-enabled Mach-O demo.

## Wire Format

Every log packet carries an 18-byte little-endian header:

| Field | Size | Description |
|---|---|---|
| `magic` | u8 | `0x9a` |
| `type_level` | u8 | High nibble: packet type, low nibble: level |
| `seq` | u16 | Sequence counter (wraps) |
| `time_ms` | u32 | Milliseconds since boot |
| `tag_id` | u32 | Tag string address in ELF |
| `fmt_id` | u32 | Format string address in ELF |
| `payload_len` | u16 | Payload bytes, or `0xffff` for streaming |

Packet types: `LOG` (0), `DROPPED` (1), `TIME_SYNC` (2), `BOOT` (3), `BUFFER` (4).

Format and tag strings are stored in ELF-only `.noload_keep_in_elf.*` sections — excluded from the flashed binary, resolved by address on the host.

The UART transport wraps packets in typed SLIP envelopes with CRC-16-CCITT verification.

## Firmware API

```c
// C macros
ON9_LOGE("wifi", "connection failed: %d", err);
ON9_LOGI("sensor", "value=%d name=%s", val, name);
ON9_LOG_BUFI("stack", buf, len);

// Runtime format pointers use the explicit runtime variant. Literal macros
// keep their format strings in the ELF-only .noload section.
const char *runtime_fmt = select_format();
ON9_LOG_RUNTIMEI("sensor", runtime_fmt, val);

// ISR-safe (no dynamic strings)
ON9_ISR_LOGW("isr", "overrun on core %d", core);

// Runtime filtering
on9log_set_level(ON9_LOG_LEVEL_WARN);
on9log_set_tag_level("wifi", ON9_LOG_LEVEL_DEBUG);

// char pointers are encoded as copied strings; use ON9_PTR() for pointer values
ON9_LOGI("debug", "name=%s ptr=%p", name, ON9_PTR(name));
```

```cpp
// C++ wrapper
#include <string>
#include <string_view>

Logger log("demo");
log.info("value={} name={}", value, name);           // printf-style
log.info<"value={} name={}">(value, name);            // no-load {} style
log.warn("status=%d", code);
std::string label = "pump";
std::string_view state = "ready";
log.info("label={} state={}", label, state);          // length-aware, no heap copy
log.buffer_info(bytes, len);
log.isr_error("isr fault core=%d", core);
```

### C argument caveat

The C macros classify arguments from their expression types before the firmware
emits the packet. `char *` and `const char *` arguments are treated as dynamic
strings and copied into the log payload for `%s` / `%.*s`. To log a character
pointer's address with `%p`, cast it to a pointer argument with `ON9_PTR(ptr)`.
The macro expands to `const void *`, which is encoded as an on9log pointer.

C `%.*s` still renders with host-side precision. By default, firmware does not
scan the original format literal because that can retain the literal in the
flashed binary in addition to the `.noload` copy. In that mode, C string
arguments are copied like `%s`: up to the first NUL byte or
`ON9LOG_MAX_DYNAMIC_STRING_LEN`. Define `ON9LOG_ENABLE_FORMAT_SCAN_HINT=1` to
enable firmware-side `%.*s` scanning and length-bounded copying. For
non-NUL-terminated slices without retaining format literals, prefer the C++
wrapper with `std::string_view`.

## Host Tools

The host-side decoder library (`on9log-protocol`), live monitor (`on9log-cli`),
and SQLite capture tool (`on9log-capture`) have moved to a separate repository. 

See: https://github.com/huming2207/on9log_host

## Project Structure

```
on9log/
├── on9log.h / on9log.c          Core packet producer (C)
├── on9log.hpp                   C++20 header-only wrapper
├── on9log_fmt.h                 Wire format definitions
├── on9log_config.h              Compile-time configuration
├── on9log_port.h                Platform abstraction layer
├── on9log_esp_port.c            ESP-IDF port (lock, timestamp)
├── on9log_esp_vfs.c/.h          SLIP+CRC VFS transport sink
├── on9log_esp_isr.c/.h          ISR ringbuffer + drain task
├── esp_stdio_log_vfs.c/.h       Shared stdio VFS framer
├── on9log_unix_port.c           Linux/macOS lock and timestamp port
├── on9log_unix_stdio.c/.h       Linux/macOS stdio sink
├── on9log_port_weak.c           Weak no-op stubs for non-ESP targets
├── CMakeLists.txt
└── Kconfig
```

## Building

On ESP-IDF, add this directory as a standard component (under `components/` or
through `EXTRA_COMPONENT_DIRS`). The Unix shim files are excluded from that
build.

### Platform isolation

The CMake file has two disjoint source lists:

- when ESP-IDF provides `idf_component_register`, it builds only the original
  ESP sources (`on9log.c`, ESP port/VFS/ISR files, the weak ISR fallbacks, and
  the stdio VFS transport);
- otherwise, standalone CMake is accepted only on Linux/macOS and builds the
  core with `on9log_unix_port.c` and `on9log_unix_stdio.c`.

The Unix shim, demos, tests, pthread dependency, Mach-O metadata, and Linux
`-no-pie` option are therefore never compiled or linked into ESP firmware.
The shared core and public embedded APIs are unchanged. Other embedded targets
still need their own `on9log_port.h` implementation and build integration;
the standalone CMake branch is intentionally a Unix host build, not a generic
cross-compilation system.

The current ESP-IDF project requires CMake 3.22, which satisfies this
component's CMake 3.20 minimum. Firmware compatibility after local edits still
needs confirmation with an actual `idf.py build`; host tests do not replace an
ESP toolchain build.

For a binary Linux/macOS host build:

```sh
cmake -S . -B build -DON9LOG_PLAIN_TEXT=OFF
cmake --build build
ctest --test-dir build --output-on-failure
./build/on9log_unix_demo > on9log.bin
```

For a plain-text host build:

```sh
cmake -S . -B build-text -DON9LOG_PLAIN_TEXT=ON
cmake --build build-text
ctest --test-dir build-text --output-on-failure
./build-text/on9log_unix_demo
```

The Unix demo mirrors the ESP32 demo's format, string-slice, C++ wrapper,
buffer, secondary-sink, runtime-filter, worker-thread, and heartbeat cases.
ESP chip/flash/FreeRTOS values are replaced with `uname`, hardware concurrency,
monotonic time, and process RSS values. Set `ON9LOG_BUILD_DEMO=OFF` when using
this directory only as a dependency.

Initialize the host sink before logging:

```c
#include "on9log_unix_stdio.h"

int main(void)
{
    if (on9log_unix_stdio_init() != ON9LOG_OK) {
        return 1;
    }
    ON9_LOGI("host", "value=%d", 42);
    return 0;
}
```

`on9log_unix_stdio_init()` writes to `stdout`. Tests or applications that need
another standard stream can call `on9log_unix_stdio_init_file(FILE *)` instead;
that stream must remain open for the process lifetime.

### Code size

Since there's no string formatter needed inside the firmware binary, the whole logging library size is around 8105 bytes on ESP32. 
It should be even much smaller than other bare metal platforms where no virtual FS layer is needed. 

## License

[WTFPL](https://en.wikipedia.org/wiki/WTFPL)
