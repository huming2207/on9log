# on9log Notes

## Goal

`on9log` is a custom logging component for this ESP32-S3 project.

The original goal was to mimic ESP-IDF Log V2 binary logging while also allowing
each log call to be forwarded as a complete packet to other transports such as
MQTT or WebSocket.

After discussing footprint and speed, the current direction is **not**
ESP-IDF Log V2 wire compatibility. Instead, `on9log` now uses a smaller,
fixed-header packet format designed for fast in-firmware emission and simple
transport forwarding.

## ESP-IDF Log V2 Notes

ESP-IDF Log V2 binary mode removes format strings from the flashed app binary by
placing them in ELF-only `.noload_keep_in_elf.*` sections. The firmware sends
format/tag addresses plus encoded arguments. A host decoder opens the matching
ELF and maps those addresses back to strings.

Important points:

- `esp_log_set_vprintf()` does not catch ESP-IDF binary logs.
- ESP-IDF binary logging writes raw bytes through its own internal output path.
- To forward binary logs in firmware, either patch ESP-IDF's binary formatter or
  implement a separate logger.
- Network I/O should not happen directly in the logging path.
- Forward or frame log packet streams into a queue/ring buffer, then let
  MQTT/WebSocket tasks drain them.

## Current Packet Format

The implemented packet format is defined in `on9log_fmt.h`.

All multi-byte fields are little-endian.

```c
#define ON9LOG_PACKET_MAGIC 0x9a

typedef enum {
    ON9LOG_PKT_LOG       = 0,
    ON9LOG_PKT_DROPPED   = 1,
    ON9LOG_PKT_TIME_SYNC = 2,
    ON9LOG_PKT_BOOT      = 3,
    ON9LOG_PKT_BUFFER    = 4,
} on9log_packet_type_t;

typedef struct {
    uint8_t  magic;       // ON9LOG_PACKET_MAGIC
    uint8_t  type_level;  // high nibble: packet type, low nibble: on9log_level_t
    uint16_t seq;         // wraps naturally
    uint32_t time_ms;     // milliseconds since boot, wraps naturally
    uint32_t tag_id;      // tag string address in ELF
    uint32_t fmt_id;      // format string address in ELF
    uint16_t payload_len; // bytes after this header, or 0xffff for streaming
} __attribute__((packed)) on9log_packet_header_t;
```

`payload_len == ON9LOG_PAYLOAD_LEN_STREAMING` means the logger is emitting a
streamed packet and the transport/sink framing determines the packet end. Sink
callbacks receive an explicit `end_cb()`. Raw UART output does not add an
extra end delimiter, so consumers that need streamed packets over UART must add
a framing layer outside this raw byte stream.

## Default ESP VFS Sink

`on9log_esp_vfs.c` provides the default framed ESP VFS sink. This VFS path
has been validated with ESP-IDF v6.0.1; ESP-IDF v6.0 had an ISR WDT issue in
this usage. It calls
`esp_stdio_log_vfs_init()`, registers an `on9log_sink_t`, buffers each raw on9log
packet until `end_cb()`, and forwards it through the shared stdio transport
framer as frame type `0x01`. Output fanout to ESP console VFS devices is owned
by `esp_stdio_log_vfs.c`:

- `/dev/uart/<CONFIG_ESP_CONSOLE_UART_NUM>` when console UART is enabled;
- `/dev/usbserjtag` when USB Serial/JTAG console output is enabled;
- `/dev/cdcacm` when USB CDC console output is enabled.

Additional outputs should be added through `esp_stdio_log_vfs_add_output(path)` before or
after `on9log_esp_vfs_init()`.

Because raw UART output has no packet boundary, the stdio VFS transport uses an
explicit-start typed SLIP envelope for both on9log binary packets and normal
plain text:

```text
0xa5
SLIP(frame_type)
SLIP(payload bytes)
SLIP(crc16_ccitt_le)
0xc0
```

Frame type values:

```text
0x01  on9log binary packet (payload is on9log header + on9log payload)
0x02  plain text stdout/stderr bytes
```

Frame start is `0xa5`; frame end is `0xc0`. Payload byte `0xa5` is escaped as
`0xdb 0xde`; payload byte `0xc0` is escaped as `0xdb 0xdc`; payload byte `0xdb`
is escaped as `0xdb 0xdd`. Because this transport is written through ESP-IDF
console VFS devices that can perform newline conversion, original byte `0x0d`
is escaped as `0xdb 0xd0` and original byte `0x0a` is escaped as `0xdb 0xd1`.
This CR/LF escaping applies to both payload bytes and CRC bytes; otherwise the
UART VFS can inject `0x0d` before `0x0a` after CRC calculation and make the host
reject the frame.

The CRC is CRC-16-CCITT with initial value `0xffff`, calculated over the
unescaped frame type byte and payload bytes. The final two CRC bytes are
appended little-endian and SLIP-escaped before the ending `0xc0`. The
implementation is LUT-based and does not use the ESP ROM CRC implementation.

`ESP_STDIO_LOG_VFS_FRAME_MAX_PAYLOAD` is currently 3072 bytes. Text writes are
chunked into multiple text frames if needed. On9log binary packets are emitted as
one transport frame; if `header + payload` exceeds the 3072-byte transport
payload cap, the VFS sink drops that UART transport frame. `ON9_LOG_BUF*()`
dumps are chunked by the core into multiple `ON9LOG_PKT_BUFFER` packets, and the
default `ON9LOG_BUFFER_CHUNK_SIZE` is 3042 bytes so each default buffer packet
fits this VFS transport cap (`3072 - 18 byte on9log header - 12 byte buffer
metadata`). Non-UART sinks such as MQTT or WebSocket still receive raw on9log
packet bytes without SLIP wrapping.

The sink takes `esp_log_impl_lock()` from `start_cb()` through `end_cb()` so
on9log packet buffering does not interleave across tasks/cores. It also takes
`flockfile(stdout)` for the same interval, flushes `stdout` before emitting the
transport frame, and releases the file lock after the frame write. Normal stdio
users such as `printf()`/`fprintf(stdout, ...)` are wrapped by
`esp_stdio_log_vfs.c` as text frames. `esp_stdio_log_vfs_write_frame()` uses a
dedicated transport mutex while writing raw bytes to the configured console fds,
so stdout, stderr, and on9log binary transport frames share one byte-stream
serialization point without relying on the ESP log lock. `on9log_esp_vfs_init()`
disables the core raw ROM UART output with
`on9log_set_uart_enabled(false)` so framed and unframed log streams are not
mixed on the console.

Normal log packets use:

```text
header
argument metadata
encoded argument payload
```

Payload encoding:

```text
uint8_t arg_count
uint8_t arg_types[arg_count]
encoded arguments in printf format order
```

Argument type values are the `ON9_LOG_ARGS_TYPE_*` values from `on9log.h`.

Encoded argument values use:

```text
32-bit argument       4 bytes
64-bit argument       8 bytes
pointer argument      4 bytes
dynamic string        uint32_t length + copied bytes, no trailing NUL
null dynamic string   uint32_t 0xffffffff, no copied bytes
```

For `%.*s`, the precision argument is still encoded as its normal 32-bit
argument before the string. The firmware does not parse no-load format strings
at runtime, so the host decoder applies the precision when rendering. Argument
emission must pass one `va_list *` through all argument helpers; passing `va_list`
by value can restart or duplicate the cursor on ESP32-S3 and causes width or
precision integers such as `%*s`/`%.*s` arguments to be read as string pointers.

The host decoder is expected to recover the format string from the ELF, parse
the format string, read the argument type table, and consume the payload
accordingly.

## Core API And Configuration

The core `on9log.h` / `on9log.c` packet producer does not depend on ESP-IDF log
types or ESP-IDF error types. It defines:

```c
typedef enum {
    ON9LOG_OK = 0,
    ON9LOG_ERR_FAIL = -1,
    ON9LOG_ERR_INVALID_ARG = -2,
    ON9LOG_ERR_NO_MEM = -3,
    ON9LOG_ERR_NOT_FOUND = -4,
    ON9LOG_ERR_INVALID_SIZE = -5,
} on9log_err_t;

typedef enum {
    ON9_LOG_LEVEL_NONE = 0,
    ON9_LOG_LEVEL_ERROR = 1,
    ON9_LOG_LEVEL_WARN = 2,
    ON9_LOG_LEVEL_INFO = 3,
    ON9_LOG_LEVEL_DEBUG = 4,
    ON9_LOG_LEVEL_VERBOSE = 5,
} on9log_level_t;
```

`ON9_LOGE/W/I/D/V()` and `ON9_LOG_BUF*()` use these on9log levels, not
`ESP_LOG_*`. C++ users can include `on9log.hpp` and use the header-only
`on9log::Logger` wrapper instead:

```c++
using namespace on9log::literals;

on9log::Logger log("demo");
log.info("value={} name={}", value, name);
log.info(ON9FMT("value={} name={}"), value, name);
log.info<"value={} name={}">(value, name);
log.info("value={} name={}"_on9fmt, value, name);
log.warn("value=%d", value);
log.buffer_info(bytes, len);
```

The wrapper calls the same `on9log_write()` / `on9log_write_buffer()` C APIs,
builds the argument type table with C++ templates, avoids heap-allocating STL
types, and is compatible with `-fno-exceptions` and `-fno-rtti`. Plain
`logger.info("...", ...)` keeps the format literal in normal read-only flash.
`ON9FMT("...")`, `logger.info<"...">(...)`, and
`"..."_on9fmt` route through attributed static storage so the format can live in
`.noload_keep_in_elf.*` instead.
`ON9_LOG_ENABLED(level)` is controlled by `ON9_LOG_LOCAL_LEVEL`; users can
define `ON9_LOG_LOCAL_LEVEL` before including `on9log.h` or `on9log.hpp` to
override the default for one translation unit.

In ESP-IDF builds, `Kconfig` exposes:

```text
CONFIG_ON9LOG_MAXIMUM_LEVEL
CONFIG_ON9LOG_MAX_SINKS
CONFIG_ON9LOG_BUFFER_CHUNK_SIZE
CONFIG_ON9LOG_MAX_DYNAMIC_STRING_LEN
CONFIG_ON9LOG_ISR_PACKET_MAX
CONFIG_ON9LOG_ESP_ISR_RINGBUF_SIZE
CONFIG_ON9LOG_ESP_ISR_DRAIN_TASK_STACK_SIZE
CONFIG_ON9LOG_ESP_ISR_DRAIN_TASK_PRIORITY
```

`on9log_config.h` maps `CONFIG_ON9LOG_MAXIMUM_LEVEL` to the default
`ON9_LOG_LOCAL_LEVEL` when the user has not defined it manually. It maps
`CONFIG_ON9LOG_BUFFER_CHUNK_SIZE` to `ON9LOG_BUFFER_CHUNK_SIZE`, defaulting to
3042 bytes, and `CONFIG_ON9LOG_MAX_DYNAMIC_STRING_LEN` to
`ON9LOG_MAX_DYNAMIC_STRING_LEN`, defaulting to 1024 bytes. Outside ESP-IDF,
`on9log_config.h` falls back to INFO level, 4 sinks, the same 3042-byte buffer
chunk size, the same 1024-byte dynamic string cap, and a 128-byte maximum ISR
packet size.

Platform-specific services are isolated behind `on9log_port.h`:

```c
void on9log_port_lock(void);
void on9log_port_unlock(void);
uint32_t on9log_port_timestamp_ms(void);
void on9log_port_write(const uint8_t *data, size_t len);
```

`on9log_port_weak.c` provides weak no-op/default implementations so the core can
link on non-ESP targets. In ESP-IDF builds it must not define
`on9log_port_lock()`, `on9log_port_unlock()`, or `on9log_port_timestamp_ms()`;
otherwise the static archive linker can satisfy those references from the weak
object before pulling in `on9log_esp_port.c`, causing all timestamps to remain
zero. `on9log_esp_port.c` provides the ESP-IDF implementation using
`esp_log_impl_lock()` and `esp_log_timestamp()`. It deliberately does not
override `on9log_port_write()`: ESP-IDF console output should be provided by
`on9log_esp_vfs.c`, which buffers a complete on9log packet and passes it to
`esp_stdio_log_vfs_write_frame()`.

Buffer dump packets use packet type `ON9LOG_PKT_BUFFER`. The `tag_id` field is
the tag pointer and `fmt_id` is zero.

```text
uint32_t total_len
uint32_t offset
uint32_t chunk_len
uint8_t  bytes[chunk_len]
```

The buffer pointer is always treated as dynamic memory and the bytes are copied
into `ON9LOG_PKT_BUFFER` packets. Large dumps are split into multiple packets
using `ON9LOG_BUFFER_CHUNK_SIZE`; each packet carries the same `total_len`, the
packet's `offset`, and that packet's `chunk_len`. The default chunk size is
chosen to fit the default ESP VFS transport, and can be overridden by
`CONFIG_ON9LOG_BUFFER_CHUNK_SIZE` or `ON9LOG_BUFFER_CHUNK_SIZE` for transports
with different limits.

## Format And Tag Strings

`ON9_LOGx()` macros place constant format strings in `.noload_keep_in_elf.*` on
ELF targets by default.

That means:

- the string remains in the ELF;
- the string is not included in the flashed app binary;
- the packet only sends `fmt_id`, the address of the format string;
- the host needs the matching ELF to decode logs.

`ON9_LOG_NOLOAD_ATTR` is overridable before including `on9log.h`. It defaults to
the ESP-IDF/ELF `.noload_keep_in_elf.<counter>` section attribute, but is empty
on Apple/Mach-O targets where that section spelling is invalid. This keeps the
core header usable outside ESP-IDF while preserving the ESP-IDF no-load behavior
for firmware builds.

`on9log.hpp` supports both flash-resident and no-load format strings. The plain
`Logger::info("...", ...)` style API sends the normal string literal pointer as
`fmt_id`; the host can still resolve it from the ELF, but the literal is kept in
the flashed binary. Use `ON9FMT("...")`, the NTTP form
`Logger::info<"...">(...)`, or the user-defined literal form
`Logger::info("..."_on9fmt, ...)` when the format should be emitted into
`.noload_keep_in_elf.*`.

Both C++ forms are still address-only on the wire. Plain
`Logger::info("...", ...)` does **not** copy the format string into the log
packet; it sends the `.rodata` address as `fmt_id`. The no-load forms also send
only an address, but that address points into the ELF-only no-load section. The
only difference is whether the format literal is present in the flashed binary.

Tags are currently passed as pointer values too. If a tag is static, the host can
resolve it from the ELF or from normal read-only sections.

## Dynamic Strings

All `char *`, `const char *`, and character-array arguments are treated as
dynamic strings by default, including direct string literals. When such an
argument is consumed by a `%s` or `%.*s` conversion, the logger copies the
string bytes into the packet as:

```text
uint32_t length
bytes[length]
```

The copied bytes do not include a trailing NUL. A null dynamic string pointer is
encoded as length `0xffffffff` with no following bytes.

Dynamic string copying is bounded by `ON9LOG_MAX_DYNAMIC_STRING_LEN`, defaulting
to 1024 bytes. If no NUL byte is found before that limit, the encoded string is
truncated to the cap. The current wire format does not include a truncation flag,
so the host sees the truncated byte sequence as the argument value. The default
cap is below the 3072-byte ESP VFS transport payload limit for common logs, but
the cap is per string argument; one log call with multiple large dynamic strings
can still exceed the default transport frame cap and be dropped by that sink.
The cap is byte-based, not UTF-8-aware. If a UTF-8 string is truncated in the
middle of a multi-byte codepoint, the host receives invalid UTF-8 bytes for that
argument and may render a replacement character.

String arguments are not emitted as ELF pointer IDs. The host distinguishes
copied dynamic strings from non-string pointers by reading the argument type
table at the start of the payload.

This makes dynamic strings self-contained in the log stream while bounding the
amount of memory scanned for a malformed or non-NUL-terminated `char *`.

## Sequence Counter

Each packet has a `uint16_t seq`.

The sequence counter answers:

```text
Did the host receive every packet, in order?
```

This is different from the timestamp, which answers:

```text
When did this happen?
```

The counter wraps naturally. Host-side gap detection can use unsigned modular
arithmetic:

```c
uint16_t expected = last_seq + 1;
if (seq != expected) {
    uint16_t missed = seq - expected;
}
```

This works across wrap, for example:

```text
65534 -> 65535 -> 0 -> 1
```

If losing more than 65535 packets between two received packets is realistic, the
counter should become `uint32_t`.

## Timestamp

The packet stores `uint32_t time_ms`, milliseconds since boot.

This wraps after about 49.7 days. Host-side elapsed-time calculation should use
unsigned modular subtraction:

```c
uint32_t dt = now_ms - previous_ms;
```

This works across wrap.

UTC should not be placed in every normal packet. A better future design is a
separate time sync packet:

```text
ON9LOG_PKT_TIME_SYNC:
    boot_time_ms
    utc_unix_ms
```

The host can then map boot-relative timestamps to UTC without increasing every
normal log packet.

## Dropped Packets

If the logger cannot build a packet, it increments an internal dropped counter.

Before the next successful normal log, it emits:

```text
ON9LOG_PKT_DROPPED
payload: uint32_t dropped_count
```

This distinguishes device-side logger drops from transport loss.

Transport loss can still be detected by sequence gaps. Device-side drops are
reported explicitly by the dropped packet.

## Current Transport Behavior

`on9log` currently supports:

- a platform `on9log_port_write()` hook for non-ESP or custom raw transports;
- registered callback sinks using `on9log_add_sink()`.

Sinks use a streaming interface:

```c
typedef struct {
    void (*start_cb)(const uint8_t *header, size_t header_len, void *ctx);
    void (*payload_cb)(const uint8_t *payload,
                       size_t payload_len,
                       size_t total_arg_cnt,
                       size_t curr_arg_index,
                       void *ctx);
    void (*end_cb)(void *ctx);
} on9log_sink_t;
```

`start_cb()` receives the fixed packet header. `payload_cb()` receives one
payload span at a time. `end_cb()` marks the end of the packet so a sink can
close/flush a ring buffer item, calculate a transport CRC, or finish an MQTT or
WebSocket frame.

For normal log packets, `total_arg_cnt` is the argument count and
`curr_arg_index` is the argument being emitted. Payload metadata, such as the
argument count and type table, is emitted with `curr_arg_index == SIZE_MAX`.
For buffer dump packets, the copied buffer bytes are emitted with
`total_arg_cnt == 1` and `curr_arg_index == 0`; buffer metadata is emitted with
`curr_arg_index == SIZE_MAX`.

Sink callbacks should be fast. They should copy/enqueue or frame the streamed
packet and return. They should not perform blocking MQTT/WebSocket/TCP I/O
directly, and should avoid logging from inside the sink to prevent recursion.

Buffer dumps are emitted with:

```c
ON9_LOG_BUFE(TAG, buffer, len);
ON9_LOG_BUFW(TAG, buffer, len);
ON9_LOG_BUFI(TAG, buffer, len);
ON9_LOG_BUFD(TAG, buffer, len);
ON9_LOG_BUFV(TAG, buffer, len);
```

## ISR Logging

Normal `ON9_LOGx()` and `ON9_LOG_BUF*()` remain task-context APIs and must not be
called from ISR context.

`ON9_ISR_LOGE/W/I/D/V()` provide a constrained ISR-safe ingress path. They encode
one complete on9log packet into a fixed stack buffer of
`ON9LOG_ISR_PACKET_MAX` bytes, defaulting to 128, and enqueue that packet through
`on9log_port_isr_enqueue_packet()`. They do not call sinks, VFS, network APIs, or
blocking UART writes directly from the ISR. Dynamic string arguments are rejected
on this path; only 32-bit, 64-bit, and pointer arguments are supported. If packet
encoding exceeds `ON9LOG_ISR_PACKET_MAX`, unsupported argument types are used, or
the initialized queue is full, the logger increments the normal dropped-packet
counter.

On ESP-IDF, `on9log_esp_isr.c` implements the ISR enqueue backend using
Espressif's `freertos/ringbuf.h` no-split ringbuffer. The user must call
`on9log_esp_isr_init()` during startup, after registering the sinks/transports
that should receive ISR logs, to create the ringbuffer and a drain task. The ISR
path checks `on9log_port_isr_ready()` first; if the backend was not initialized,
`ON9_ISR_LOGx()` is a no-op and does not increment the dropped counter. Once
initialized, the ISR side uses `xRingbufferSendFromISR()` and returns immediately
if there is not enough space. The drain task calls `on9log_dispatch_packet()` in
normal task context, so configured sinks such as the default VFS sink, MQTT, or
WebSocket forwarders still receive complete on9log packets without doing network
or VFS I/O inside the ISR. Because ESP-IDF no-split ringbuffers reserve item
overhead and cannot use the whole buffer for a single item, keep
`CONFIG_ON9LOG_ESP_ISR_RINGBUF_SIZE` comfortably larger than twice
`CONFIG_ON9LOG_ISR_PACKET_MAX`.

This ESP ringbuffer backend is intentionally platform-specific. Future
non-ESP ports should implement `on9log_port_isr_enqueue_packet()` using the
platform's own ISR-safe queue, lock-free ring, or interrupt-safe handoff
primitive.

`ON9_ISR_LOGx()` is ISR-safe in the normal FreeRTOS sense, but it is not
guaranteed IRAM-safe or flash-cache-disabled safe on ESP-IDF. The current path is
not marked `IRAM_ATTR`, and marking only the public function would be
misleading: every helper it calls and every data object it reads, directly or
indirectly, would also need to live in IRAM/DRAM. ESP interrupts registered with
`ESP_INTR_FLAG_IRAM` should not use this path unless a future dedicated
IRAM/DRAM-safe variant is added and audited end-to-end.

The C++ `on9log::Logger` wrapper exposes matching
`isr_error/warn/info/debug/verbose()` methods that call `on9log_write_isr()`.
They are also header-only templates and reject dynamic string arguments at
compile time, matching the C ISR path's runtime restriction.

## Locking And Atomics

The logger hot path should avoid locks where practical.

Current shared state is handled as follows:

- packet sequence counter: atomic;
- dropped packet counter: atomic;
- UART enable flag: atomic;
- sink stream dispatch: lock-free atomic pointer loads;
- sink add/remove: still locked to serialize table mutation and duplicate/free
  slot checks.

Sink slots publish `ctx` first and then publish the sink-struct pointer with
release semantics. Dispatch loads `sink` with acquire semantics and only reads
`ctx` if `sink` is non-null. At packet start, dispatch snapshots the currently
registered sink pointers and contexts, then uses that snapshot for all payload
callbacks and `end_cb()` for that packet. The sink struct must have static or
otherwise long-lived storage; the logger stores its pointer, not a copy.

This makes normal log emission and sink dispatch lock-free. Registration and
removal are expected to be infrequent, so keeping those paths locked is a
reasonable tradeoff.

One important lifetime rule:

```text
After on9log_remove_sink(), do not immediately free ctx or the sink struct if
another core may already have loaded the callback pointer.
```

Removal prevents future dispatches after the cleared sink pointer is observed,
but it is not an in-flight callback barrier. A sink already captured in a packet
snapshot may still receive payload callbacks and `end_cb()`. If strict teardown
is needed later, add reference counting, an epoch/RCU scheme, or make sinks
init-only.

## clangd Atomic Diagnostics

The firmware builds with ESP-IDF's Xtensa GCC toolchain, but editors often parse
the code with `clangd`. `clangd` can report warnings around C11 `<stdatomic.h>`
macros such as:

```text
Incompatible pointer types passing 'typeof ((void)0 , *__atomic_store_ptr) *'
to parameter of type 'atomic_bool *'
```

This is caused by Clang checking GCC/ESP-IDF atomic macro expansions more
strictly than the actual project compiler. If `idf.py build` passes, this is an
editor analysis issue rather than a firmware build failure.

Practical options:

- ignore the `clangd` false positive if the ESP-IDF build passes;
- ensure `clangd` uses the generated `compile_commands.json` from the ESP-IDF
  build directory;
- replace C11 `<stdatomic.h>` calls with GCC `__atomic_*` builtins on plain
  scalar fields if the editor diagnostics become too noisy.

The GCC builtin style looks like:

```c
static bool s_uart_enabled = true;

__atomic_store_n(&s_uart_enabled, enabled, __ATOMIC_RELAXED);
bool enabled = __atomic_load_n(&s_uart_enabled, __ATOMIC_RELAXED);
```

This keeps atomic code generation while usually avoiding `clangd`'s `_Atomic`
type confusion.

## Known Implementation Risks

The current implementation is usable for the intended ESP32-S3 experiment, but
these review findings are still open and should be treated as real constraints:

- **Non-constant format strings are not rejected.** `ON9_LOG_ATTR_STR(str)` falls
  back to `(str)` when `__builtin_constant_p(str)` is false. Such logs can still
  compile, but the firmware only sends the pointer as `fmt_id`; the host resolver
  expects format strings in `.noload` and will render unresolved dynamic formats
  as `<fmt @0x........>`.
- **Argument capture is ABI-reliant.** Type detection only special-cases a small
  set of C/C++ types, then `on9log_emit_arg()` reads raw `uint32_t`, `uint64_t`,
  pointer, or string values from `va_arg`. This is not a complete printf ABI
  model: signed integer promotion, `double`/`float`, arbitrary pointer types,
  and using `char *` with `%p` are still sharp edges.
- **Public `on9log_write()` trusts `arg_types`.** The macro-generated type table
  is NUL-terminated, but direct callers can pass a non-terminated or inconsistent
  table. `on9log_arg_count()` will scan until `ON9_LOG_ARGS_TYPE_NONE` or
  `UINT8_MAX`, so external callers must provide a valid table.
- **Some VFS output errors are still hidden.** `esp_stdio_log_vfs_add_output()`
  returns `ESP_OK` even if `open()` fails, and the on9log VFS sink currently
  ignores the return value from `esp_stdio_log_vfs_write_frame()`. The shared
  framed write path now handles short writes and reports failure to the stdio VFS
  callback, but binary UART drops still surface mainly as host-side sequence gaps.
- **Init failure has no rollback.** `esp_stdio_log_vfs_init()` can register the
  VFS and redirect `stdout` before a later failure, and `on9log_esp_vfs_init()`
  can fail after stdio redirection but before disabling the raw UART path. Failed
  init should be considered potentially partially applied.
- **Sink removal is not an in-flight callback barrier.** The lifetime rule in the
  locking section is part of the API contract until teardown synchronization is
  added.
- **Firmware build has not been verified in this shell.** Rust host tests and
  clippy passed, but `idf.py` was not available here, so C-side compatibility must
  still be confirmed with the ESP-IDF build environment.

## Current Tradeoffs

Compared with ESP-IDF Log V2 binary packets, this format is:

- faster to emit;
- smaller and simpler;
- little-endian, matching ESP32-S3 native byte order;
- not compatible with ESP-IDF monitor's binary log decoder;
- dependent on a custom host decoder.

The host decoder must:

- validate `magic`;
- parse `type_level`;
- track `seq`;
- handle timestamp wrap;
- map `fmt_id` and `tag_id` through the ELF;
- parse the format string;
- read the payload argument type table;
- decode pointer, scalar, and copied dynamic-string arguments;
- use sink/transport framing to find the end of streamed packets;
- decode `ON9LOG_PKT_BUFFER` copied memory dump bytes;
- pass through verified type `0x02` text and printable raw UART text, preserving
  device-emitted ANSI bytes rather than inferring colors from text prefixes;
- recognize ESP panic text (`abort() was called`, `Backtrace:`, Guru
  Meditation, assertions, stack canary messages) and annotate crash PCs/backtrace
  entries using the matching ELF/DWARF data when available;
- reset ESP targets on monitor startup by default with DTR/RTS, unless the user
  passes `--no-esp-reset`.
