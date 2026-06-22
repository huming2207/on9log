# on9log host_cli Notes

This crate (`on9log_host`) is the host-side decoder and CLI for the `on9log`
binary log stream produced by the C component in the parent directory. It is a
Rust **library + binary** crate:

- the `on9log_host` library implements the pure decoding pipeline (no runtime
  dependency) so it can be reused by a future `napi-rs` binding for programmatic
  access;
- the `on9log` binary is a CLI that opens a UART port, decodes the stream, and
  prints colorized, terminal-width-wrapped log lines.

See the parent `../AGENTS.md` for the firmware-side packet format, SLIP framing,
CRC, and ELF string strategy. This document only describes the host side.

## Goal

Given a UART byte stream emitted by `on9log_esp_vfs.c`, recover individual
packets, verify them, decode arguments, resolve format/tag strings from the
matching firmware ELF, and render human-readable colored log output that wraps
to the current terminal width.

The decoder must also be usable as a library (e.g. from a Node.js binding via
`napi-rs`), so the decoding core is kept free of any I/O / runtime dependency.
Only the CLI binary pulls in `tokio` (the runtime `tokio-serial` requires).

## Wire Format Recap (host view)

All multi-byte fields are little-endian. The packet header is **18 bytes**
(`on9log_packet_header_t` in `on9log_fmt.h`):

```text
magic        u8   0x9a
type_level   u8   high nibble: packet type, low nibble: esp_log_level_t
seq          u16  wraps naturally
time_ms      u32  milliseconds since boot, wraps naturally
tag_id       u32  tag string address in ELF
fmt_id       u32  format string address in ELF
payload_len  u16  bytes after header, or 0xffff for streaming
```

Packet types (`ON9LOG_PKT_*`):

```text
0  LOG
1  DROPPED
2  TIME_SYNC
3  BOOT
4  BUFFER
```

Log levels (`esp_log_level_t`):

```text
0 NONE    1 ERROR    2 WARN    3 INFO    4 DEBUG    5 VERBOSE
```

The ESP VFS sink frames each packet over raw UART with SLIP + a trailing
CRC-16-CCITT:

```text
0xc0
SLIP(header bytes)
SLIP(payload bytes)
SLIP(crc16_ccitt_le)
0xc0
```

SLIP escaping:

```text
0xc0 -> 0xdb 0xdc
0xdb -> 0xdb 0xdd
```

CRC is CRC-16-CCITT (CCITT-FALSE): polynomial `0x1021`, initial value `0xffff`,
no reflection, no final xor. It is computed over the unescaped header + payload
only; the two resulting bytes are appended little-endian and then SLIP-escaped
before the closing `0xc0`. The firmware uses a 256-entry LUT.

A normal LOG packet payload is:

```text
uint8_t arg_count
uint8_t arg_types[arg_count]
encoded arguments in printf format order
```

Argument type values (`ON9_LOG_ARGS_TYPE_*`):

```text
0 NONE           1 32BITS    2 64BITS    3 POINTER    4 DYNAMIC_STRING
```

Encoded argument values:

```text
32-bit argument       4 bytes
64-bit argument       8 bytes
pointer argument      4 bytes (32-bit address)
dynamic string        uint32_t length + bytes[length], no trailing NUL
null dynamic string   uint32_t 0xffffffff, no following bytes
```

For `%.*s`, the precision argument is encoded as a normal 32-bit argument before
the string argument. The firmware does not parse no-load format strings at
runtime, so the host applies the precision when rendering.

A BUFFER packet payload is:

```text
uint32_t total_len
uint32_t offset
uint32_t chunk_len
uint8_t  bytes[chunk_len]
```

A DROPPED packet payload is `uint32_t dropped_count` (device-side logger drops,
distinct from transport loss detected by sequence gaps).

## Crate Layout

```text
host_cli/
  Cargo.toml         lib + bin; deps: clap, tokio-serial, goblin, tokio,
                     crossterm (terminal size), sprintf (printf rendering)
  src/
    lib.rs           crate root, public re-exports
    wire.rs          header/types/levels/arg-type constants and Header::parse
    crc.rs           CRC-16-CCITT-FALSE (table-generated to match firmware LUT)
    framer.rs        streaming SLIP deframer + CRC verification -> RawFrame
    elf_resolv.rs    goblin-based address -> C-string resolver
    printf.rs        printf rendering via the `sprintf` crate (+ minimal scan)
    decode.rs        stateful Decoder: arg decode + packet dispatch + seq gaps
    term.rs          terminal width via `crossterm` + ANSI colors + word wrap
    main.rs          CLI binary (clap + tokio-serial)
```

### Library public surface

```rust
use on9log_host::{Deframer, Decoder, DecodedPacket, ElfStrings, Outcome};

let mut deframer = Deframer::new();
let mut decoder = Decoder::new();
let elf = ElfStrings::from_bytes(&elf_bytes).ok();

for outcome in deframer.feed(&uart_bytes) {
    if let Outcome::Frame(frame) = outcome {
        let pkt = decoder.decode(&frame, elf.as_ref());
        // match on DecodedPacket::Log / Buffer / Dropped / Other / Malformed
    }
}
```

The decoding pipeline is intentionally synchronous and allocation-light so a
`napi-rs` binding can expose `Deframer`, `Decoder`, and `ElfStrings` directly
and feed chunks from any transport.

### Module responsibilities

`wire.rs` mirrors `on9log_fmt.h` and the `ON9_LOG_ARGS_TYPE_*` values from
`on9log.h`: `PacketType`, `Level`, `ArgType`, and `Header::parse` (returns
`None` on magic mismatch or unknown type/level nibbles).

`crc.rs` computes CRC-16-CCITT-FALSE the same way the firmware's LUT does
(`crc = (crc << 8) ^ table[((crc >> 8) ^ byte) & 0xff]`). The table is generated
at runtime from polynomial `0x1021`; the standard check value "123456789" ->
`0x29b1` is covered by a unit test.

`framer.rs` holds the streaming `Deframer`. Feed it arbitrary byte slices. Bytes
seen outside a SLIP frame are accumulated as third-party/plain-text output and
returned as `Outcome::PlainText(Vec<u8>)` when the next `0xc0` is observed.
Seeing `0xc0` outside a frame is treated as an on9log frame start; bytes are then
SLIP-unescaped until the next `0xc0`, which ends the frame and triggers CRC and
header validation. `decode_frame` returns one of:

- `Outcome::Frame(RawFrame)` — header parsed, CRC verified;
- `Outcome::PlainText(Vec<u8>)` — non-on9log text bytes seen before a frame;
- `Outcome::BadMagic` — magic byte not `0x9a`;
- `Outcome::CrcMismatch` — CRC did not verify;
- `Outcome::Truncated` — fewer than header + CRC bytes;
- `Outcome::LengthMismatch` — non-streaming payload length did not match the
  header's declared `payload_len`.

Non-Frame outcomes never halt decoding. Bad SLIP frames are reported and the
deframer returns to plain-text mode after the closing `0xc0`. For non-streaming
packets (`payload_len != 0xffff`) the deframer also checks that the payload
length matches the declared value.

`elf_resolv.rs` parses the firmware ELF with goblin and builds an
address-indexed table of every section that has both a non-zero virtual address
and on-file bytes (skipping NOBITS/SHT_NOBITS sections, which carry no strings).
`read_cstr(addr)` binary-searches for the section whose `[addr, end)` range
contains the address and returns the NUL-terminated UTF-8 string. This resolves
both format strings (in `.noload_keep_in_elf.*` sections) and tag strings (in
normal read-only sections). When no ELF is supplied, addresses render as
`@0x........`.

`printf.rs` renders a format string against decoded `Arg` values using the
`sprintf` crate (`sprintf::vsprintf`), which performs all digit conversion,
padding, hex/float formatting, etc. Two things require a small amount of glue
around `vsprintf`:

- `sprintf` dispatches by the *Rust type* of each argument (via downcast) and is
  strict: `%d` wants a signed int, `%u`/`%x` an unsigned int, `%c` a `u32`, `%f`
  an `f64`, `%p` a raw pointer, `%s` a string. The on9log wire only carries raw
  32/64-bit values without signedness, so the wire alone cannot pick the right
  Rust type. `printf.rs` scans the format string to recover each conversion
  character and coerces each wire argument to the matching Rust type. `sprintf`'s
  own parser collapses `d`/`i`/`u` into one `ConversionType` variant, so it
  cannot make this call itself.
- `sprintf` 0.4.3 mishandles dynamic precision (`%.*s`): the `*` argument is
  rejected for every integer type. `printf.rs` works around this by resolving
  `*` width/precision arguments to literal values while scanning, so `vsprintf`
  only ever receives value arguments. Negative width means left-justify (C
  semantics); negative precision is omitted.

The scanner does no formatting itself. `%.*s` consumes the precision argument
before the string argument, matching the firmware's argument ordering. Null
dynamic strings render as `(null)`.

`decode.rs` owns the stateful `Decoder`. It tracks `last_seq` and reports
sequence gaps using wrapping arithmetic (`gap = seq - (last + 1)`, correct across
`u16` wrap). It dispatches each packet type:

- LOG -> parse arg type table, decode args, resolve tag/fmt, render message;
- BUFFER -> parse total_len/offset/chunk_len, carry the raw bytes;
- DROPPED -> parse the dropped count;
- TIME_SYNC / BOOT -> surfaced as `DecodedPacket::Other` with the raw payload
  (the firmware does not currently emit these, but they are reserved for forward
  compatibility);
- any decode failure -> `DecodedPacket::Malformed { meta, reason }`.

`Decoder::reset()` clears sequence tracking (e.g. after a device reboot).

`term.rs` handles presentation. Terminal width is detected with
`crossterm::terminal::size()`, which performs the platform-specific
`ioctl(TIOCGWINSZ)` on Linux and macOS for us; it falls back to the `COLUMNS`
env var, then a default of 80. Colors are plain ANSI SGR codes. `wrap()`
word-wraps to a column count (hard-breaking tokens longer than the width), and
`print_log_line()` prints a colored prefix + first line wrapped to the remaining
width, then indents continuation lines under the message. Color emission is
suppressed when stdout is not a TTY (or when `--no-color` is passed).

`main.rs` is the CLI. It uses clap with derive: `-p/--port` (required), `-b/--baud`
(default 115200), `--elf` (optional firmware ELF path), `--no-color`, and
`--width` (0 = auto-detect). It loads the ELF up front, builds a tokio runtime,
opens the serial port via `tokio_serial::new(port, baud).open_native_async()`,
and reads in a 4096-byte loop, feeding each chunk to the deframer. Plain-text
outcomes are written to stdout as-is. Verified on9log frames are decoded and
printed with the normal colored/wrapped presentation.

## Output Format

A decoded LOG line looks like:

```text
I (   1234) TAG: rendered message goes here
```

where `I` is the ESP-IDF level letter, the parenthesized number is `time_ms`
since boot, then the tag, then the rendered message. Continuation lines (when
the message wraps) are indented under the message.

Level colors:

```text
ERROR   red
WARN    yellow
INFO    green
others  white  (DEBUG, VERBOSE, NONE)
```

Buffer-dump packets print a header line followed by a hex+ASCII dump.
DROPPED packets and sequence gaps print dim yellow warning lines. Malformed
frames and undecodable payloads print diagnostics to stderr and do not stop
decoding.

## Decisions And Tradeoffs

- **18-byte header.** AGENTS.md describes the header as "fixed-header" without
  restating the size; the packed `on9log_packet_header_t` is
  1+1+2+4+4+4+2 = 18 bytes, so `HEADER_LEN = 18`.
- **Library / binary split.** The decoding core has no `tokio` dependency; only
  the binary needs the runtime. This keeps a future `napi-rs` binding small and
  sync-friendly.
- **goblin for ELF.** Resolves `fmt_id`/`tag_id` addresses to C strings by
  indexing every section with a virtual address and file bytes (covers both
  `.noload_keep_in_elf.*` format strings and normal tag sections). goblin is the
  user-chosen parser.
- **sprintf for printf rendering.** Format strings are rendered by the `sprintf`
  crate, not a hand-rolled renderer. Because `sprintf` dispatches by Rust type
  and the wire carries no signedness, `printf.rs` scans the format to coerce each
  argument to the correct type (`i8`/`u8` for `hh`, `i16`/`u16` for `h`,
  `i32`/`u32` for default/32-bit ESP32 `long`, `i64`/`u64` for `ll`, `f64` for
  floats, raw pointer for `%p`, `String` for `%s`). It also resolves `*`
  width/precision to literals to work around a `sprintf` 0.4.3 bug where `%.*s`
  rejects its `*` argument. The scanner does no formatting.
- **crossterm for terminal size.** Window width comes from
  `crossterm::terminal::size()` (cross-platform `ioctl`); colors remain raw ANSI
  SGR codes emitted inline so wrapped lines stay colored.
- **Streaming deframer.** The deframer accepts arbitrary byte boundaries,
  passes through plain text outside SLIP frames, and accumulates from a starting
  `0xc0` until a frame-ending `0xc0`, so partial reads and split frames are
  handled naturally.
- **Lenient recovery.** Bad magic, CRC mismatch, and truncation are reported but
  do not halt the stream; the next `0xc0` resynchronizes.
- **printf scope.** Rendering is delegated to `sprintf`, which covers the
  conversions used by ESP-IDF-style log formats. On a `sprintf` error the format
  string is returned with a marker so the log line is not lost.
- **Cross-platform.** Linux and macOS are supported; `crossterm` handles the
  platform-specific terminal-size syscall. Other platforms fall back to the
  `COLUMNS` env var / default width.

## Building And Running

```bash
cd host_cli
cargo build --release

# basic usage
./target/release/on9log -p /dev/ttyUSB0 -b 115200

# with ELF string resolution
./target/release/on9log -p /dev/ttyUSB0 -b 115200 --elf ../build/myapp.elf
```

CLI flags:

```text
-p, --port <PORT>     UART device path (e.g. /dev/ttyUSB0)        required
-b, --baud <BAUD>     baud rate                                    default 115200
    --elf <FILE>      firmware ELF for format/tag string resolution
    --no-color        disable colored output
    --width <WIDTH>   override terminal width (0 = auto-detect)    default 0
```

Tests:

```bash
cargo test       # CRC, sprintf rendering, SLIP/plain-text framing, decode tests
cargo clippy     # 0 warnings
```

## Future Work

- `napi-rs` binding exposing `Deframer`, `Decoder`, `ElfStrings`, and the
  decoded record types to Node.js.
- TIME_SYNC / BOOT packet decoding once the firmware emits them (currently
  surfaced as opaque `Other` payloads). Time sync would let the host map
  boot-relative `time_ms` to UTC.
- Optional sequence-gap statistics / loss-rate reporting.
- Optional JSON output mode for piping into other tooling.
- TCP / WebSocket transport sources alongside UART (the decoder is transport-
  agnostic; only `main.rs` is UART-specific).
