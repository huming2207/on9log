# on9log

Custom binary logging library for embedded platforms. Currently targets ESP32-S3; designed for portability to other MCUs and RTOSes.

The host-side decoder library and CLI tools are at: https://github.com/huming2207/on9log_host

## Overview

`on9log` provides a compact binary packet format for embedded logging, optimized for:
- fast in-firmware emission with minimal CPU overhead;
- transport-agnostic forwarding (UART, MQTT, WebSocket);
- small on-flash footprint via ELF-only format strings.

The firmware sends only addresses for format and tag strings. The matching host decoder (separate repo) resolves them from the firmware ELF, parses printf- or `{}`-style format strings, and renders colorized human-readable output.

## Architecture

```
┌─────────────────────────────────┐
│  Firmware (C + C++)              │
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

on9log::Logger log("demo");
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
├── on9log_port_weak.c           Weak no-op stubs for non-ESP targets
├── CMakeLists.txt
└── Kconfig
```

## Building

On ESP-IDF, add as a standard component (place in `components/` or `EXTRA_COMPONENT_DIRS`). On other platforms, implement the hooks in `on9log_port.h` (see `on9log_port_weak.c` for defaults) and link `on9log.c` directly.

## License

[WTFPL](https://en.wikipedia.org/wiki/WTFPL)
