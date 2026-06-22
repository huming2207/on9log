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
- ESP-IDF binary logging writes raw bytes directly through ROM serial output.
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
    uint8_t  type_level;  // high nibble: packet type, low nibble: log level
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
at runtime, so the host decoder applies the precision when rendering.

The host decoder is expected to recover the format string from the ELF, parse
the format string, read the argument type table, and consume the payload
accordingly.

Buffer dump packets use packet type `ON9LOG_PKT_BUFFER`. The `tag_id` field is
the tag pointer and `fmt_id` is zero.

```text
uint32_t total_len
uint32_t offset
uint32_t chunk_len
uint8_t  bytes[chunk_len]
```

The buffer pointer is always treated as dynamic memory and the bytes are copied
into the streamed packet. The logger no longer imposes a fixed packet-size
chunk limit. Transports may still split, buffer, reject, or frame the stream
according to their own limits.

## Format And Tag Strings

`ON9_LOGx()` macros place constant format strings in `.noload_keep_in_elf.*`.

That means:

- the string remains in the ELF;
- the string is not included in the flashed app binary;
- the packet only sends `fmt_id`, the address of the format string;
- the host needs the matching ELF to decode logs.

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

String arguments are not emitted as ELF pointer IDs. The host distinguishes
copied dynamic strings from non-string pointers by reading the argument type
table at the start of the payload.

This makes dynamic strings self-contained in the log stream. Long strings are
no longer rejected by a logger packet-size cap; transport-specific limits are
handled by sinks.

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

- UART output through ROM serial output when enabled;
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
- decode `ON9LOG_PKT_BUFFER` copied memory dump bytes.
