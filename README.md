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
| `payload_len` | u16 | Payload bytes after the header (`0xffff` is reserved for "unknown") |

Packet types: `LOG` (0), `DROPPED` (1), `TIME_SYNC` (2), `BOOT` (3), `BUFFER` (4).

`BOOT` is sent by the application once after startup with
`on9log_emit_boot()`; the reason comes from `on9log_port_reset_reason()`. Its
payload is:

| Field | Size | Description |
|---|---|---|
| `boot_id` | u32 | Random number picked once per boot |
| `reset_reason` | u8 | `on9log_reset_reason_t` |
| `wire_format_version` | u8 | `ON9LOG_WIRE_FORMAT_VERSION`, currently 1 |
| `image_id_kind` | u8 | `on9log_image_id_kind_t`: 0 none, 1 ELF SHA-256, 2 GNU build-id, 3 Mach-O UUID |
| `image_id_len` | u8 | 0–32 |
| `image_id` | bytes | The ID itself |

The image ID lets the host refuse to decode with the wrong ELF. It computes
the same value from its ELF: the SHA-256 of the whole file (kind 1), the
`NT_GNU_BUILD_ID` note (kind 2), or the `LC_UUID` load command (kind 3). On
ESP-IDF it is the full `esp_app_desc.app_elf_sha256`: the ELF cannot contain
its own hash, so `esptool elf2image` hashes the finished ELF and patches the
result into the flashed `.bin`. An image flashed without that step still holds
zeros and reports no ID. Linux reports the GNU build-id and macOS the Mach-O
UUID.

The ID is self-reported over an unauthenticated stream. It catches mistakes,
not attackers: anyone able to tamper with the stream can send the expected ID.
Firmware authenticity needs Secure Boot, and trustworthy logs would need
signed or MAC'd packets, which on9log does not provide. `TIME_SYNC` (payload: `u32 boot_id`) is meant for
batching transports: `on9log_encode_time_sync()` builds one to append to each
batch, so a receiver can place the batch in time without trusting the device
clock. `boot_id` is a random number picked once per boot.

Format and tag strings are stored in ELF-only `.noload_keep_in_elf.*` sections — excluded from the flashed binary, resolved by address on the host.

A tag or format the host cannot resolve by address (built on the stack, held
in RAM, or a runtime format) is copied into the packet instead: its
`tag_id`/`fmt_id` is `0xffffffff` (`ON9LOG_ID_INLINE`) and the payload starts
with the inline tag, then the inline format, each as `u32 length + bytes`. The
platform decides per pointer through `on9log_port_addr_in_image()`; see
[Porting](#porting).

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

### C++ format literals in .noload (C++26 reflection)

By default a plain `log.info("...")` keeps its format literal in flash; the
NTTP (`log.info<"...">()`) and `"..."_on9fmt` forms move it to `.noload`.
Enable `CONFIG_ON9LOG_CXX_REFLECTION` (or `-DON9LOG_CXX_REFLECTION=ON` for the
host build) and the plain form moves it too, with no macro: the format
parameter has a `consteval` constructor that uses C++26 reflection to
instantiate the `.noload` copy of the literal it receives. Identical formats
share one copy.

- Needs a compiler with C++26 reflection: GCC 16+ (Espressif esp-16.x) with
  `-std=gnu++26 -freflection`; the component adds `-freflection` for you.
  ESP-IDF v6.1 ships esp-15.2 and does not build with GCC 16, so use an
  ESP-IDF release that ships esp-16 (currently master).
- With an older compiler the build stops with an `#error`. It never falls back
  to keeping literals in flash.
- Plain overloads then accept only string literals and `constexpr` char
  arrays. Wrap a format chosen at runtime:

```cpp
log.info(on9log::runtime_format(pick_format()), value);
```

`on9log::runtime_format()` also exists when the option is off, so the same
code builds either way.

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

### Porting

A new platform implements the hooks in `on9log_port.h`; anything it leaves out
falls back to the weak defaults in `on9log_port_weak.c`. For inline strings,
`on9log_port_addr_in_image()` should return true only for addresses inside the
image's read-only data or `.noload_keep_in_elf` section. The weak default
returns true for everything, which sends every address as before.

On a bare-metal GNU ld target (for example STM32), the linker script must also
keep the no-load strings in the ELF without loading them, and export the
bounds the hook compares against, along with symbols around the flash
`.rodata` output section:

```ld
.noload 0 (INFO) :
{
    . = 0;
    LONG(0); /* keep address 0 free for NULL */
    _noload_keep_in_elf_start = ABSOLUTE(.);
    KEEP(*(.noload_keep_in_elf .noload_keep_in_elf.*))
    _noload_keep_in_elf_end = ABSOLUTE(.);
}
```

For the BOOT image ID, a bare-metal port should link with
`-Wl,--build-id=sha1`, keep the note in flash, and pass it to
`on9log_find_gnu_build_id()` from `on9log_port_image_id()`, reporting
`ON9LOG_IMAGE_ID_GNU_BUILD_ID`:

```ld
.note.gnu.build-id : ALIGN(4)
{
    __on9log_build_id_start = .;
    KEEP(*(.note.gnu.build-id))
    __on9log_build_id_end = .;
} > FLASH
```

### Code size

Since there's no string formatter needed inside the firmware binary, the whole logging library size is around 8105 bytes on ESP32. 
It should be even much smaller than other bare metal platforms where no virtual FS layer is needed. 

## License

[WTFPL](https://en.wikipedia.org/wiki/WTFPL)
