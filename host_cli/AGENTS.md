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

Given a UART byte stream emitted by `esp_stdio_log_vfs.c` /
`on9log_esp_vfs.c`, recover individual typed transport frames, verify them,
decode on9log binary packets, resolve format/tag strings from the matching
firmware ELF, and render human-readable colored log output that wraps to the
current terminal width.

The decoder must also be usable as a library (e.g. from a Node.js binding via
`napi-rs`), so the decoding core is kept free of any I/O / runtime dependency.
Only the CLI binary pulls in `tokio` (the runtime `tokio-serial` requires).

## Wire Format Recap (host view)

All multi-byte fields are little-endian. The packet header is **18 bytes**
(`on9log_packet_header_t` in `on9log_fmt.h`):

```text
magic        u8   0x9a
type_level   u8   high nibble: packet type, low nibble: on9log_level_t
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

Log levels (`on9log_level_t`):

```text
0 NONE    1 ERROR    2 WARN    3 INFO    4 DEBUG    5 VERBOSE
```

The ESP VFS transport frames both on9log binary packets and plain-text
stdout/stderr bytes over raw UART with an explicit-start typed SLIP envelope and
a trailing CRC-16-CCITT:

```text
0xa5
SLIP(frame_type)
SLIP(payload bytes)
SLIP(crc16_ccitt_le)
0xc0
```

Transport frame types:

```text
0x01  on9log binary packet (payload is the 18-byte on9log header + payload)
0x02  plain text stdout/stderr bytes
```

SLIP escaping:

```text
0xa5 -> 0xdb 0xde
0xc0 -> 0xdb 0xdc
0xdb -> 0xdb 0xdd
```

CRC is CRC-16-CCITT (CCITT-FALSE): polynomial `0x1021`, initial value `0xffff`,
no reflection, no final xor. It is computed over the unescaped frame type byte
and payload bytes only; the two resulting bytes are appended little-endian and
then SLIP-escaped before the closing `0xc0`. The firmware uses a 256-entry LUT.

The transport payload cap is 3072 bytes. Text writes can arrive as multiple text
frames. On9log binary packets that exceed the cap are dropped by the UART/VFS
transport; the next received on9log packet should show a sequence gap.

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
                     crossterm (terminal size), sprintf (printf rendering),
                     libc (local timestamp formatting on Unix)
  src/
    lib.rs           crate root, public re-exports
    wire.rs          header/types/levels/arg-type constants and Header::parse
    crc.rs           CRC-16-CCITT-FALSE (table-generated to match firmware LUT)
    framer.rs        typed transport SLIP deframer + CRC verification -> RawFrame/plain text
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
seen outside a transport frame are ignored; this prevents a missed start marker
from dumping binary packet debris to the terminal. Seeing `0xa5` starts a
transport frame. Bytes are then SLIP-unescaped until `0xc0`, which ends the frame
and triggers CRC/type validation. An unescaped `0xa5` inside a frame is treated
as a resync point: the partial frame is discarded and a fresh frame begins.
`decode_frame` returns one of:

- `Outcome::Frame(RawFrame)` — header parsed, CRC verified;
- `Outcome::PlainText(Vec<u8>)` — verified transport frame type `0x02`;
- `Outcome::BadMagic` — magic byte not `0x9a`;
- `Outcome::CrcMismatch` — CRC did not verify;
- `Outcome::Truncated` — fewer than header + CRC bytes;
- `Outcome::LengthMismatch` — non-streaming payload length did not match the
  header's declared `payload_len`;
- `Outcome::FrameTooLong` — decoded transport frame exceeded 3072 bytes;
- `Outcome::UnknownFrameType(_)` — verified frame type was not `0x01` or `0x02`;
- `Outcome::InvalidEscape` — bad SLIP escape sequence.

Non-Frame outcomes never halt decoding. Bad SLIP frames are reported and the
deframer returns to start-marker hunt mode. For non-streaming on9log packets
(`payload_len != 0xffff`) the deframer also checks that the payload length
matches the declared value.

`elf_resolv.rs` parses the firmware ELF with goblin and builds an
address-indexed table of string-bearing sections. It skips NOBITS/SHT_NOBITS
sections, which carry no file bytes. It normally skips VMA-0 sections too, but
keeps VMA-0 sections whose name contains `.noload`, because ESP-IDF's no-load
format-string output section is kept in the ELF with file bytes at address 0.

Format and tag resolution are intentionally separate:

- `read_format(addr)` only reads from section names containing `.noload`;
- `read_tag(addr)` reads from non-`.noload` sections.

If multiple matching sections cover the same address and produce different
strings, lookup fails instead of silently choosing one. When no ELF is supplied,
or lookup fails, addresses render as `@0x........` / `<fmt @0x........>`.

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
(default 115200), `--elf` (optional firmware ELF path), `--no-color`,
`-t/--timestamp`, and `--width` (0 = auto-detect). It loads the ELF up front,
builds a tokio runtime, opens the serial port via
`tokio_serial::new(port, baud).open_native_async()`, and reads in a 4096-byte
loop, feeding each chunk to the deframer. Verified plain-text transport frames
are written to stdout as-is unless `--timestamp` is set, in which case a local
wall-clock prefix is inserted at each text line start. Verified on9log frames are
decoded and printed with the normal colored/wrapped presentation.

## ELF No-Load String Resolution

Format and tag strings take **different ELF addresses**, which is why the
resolver splits lookup by section family rather than doing one generic scan:

- **`fmt_id`** — `ON9_LOG_NOLOAD_STR(format)` wraps constant format strings in
  `ON9_LOG_NOLOAD_ATTR`, which emits input sections `.noload_keep_in_elf.<n>`.
  ESP-IDF's linker captures these into a dedicated ELF-only output section, so
  `fmt_id` is a **small offset near 0**, not a loadable rodata address.
- **`tag_id`** — tags are passed straight through (`ON9_LOG_LEVEL(tag, ...)`)
  and land in ordinary `.rodata`, so `tag_id` is a real loadable address
  (e.g. `0x3f4xxxxx` on ESP32-S3).

The ESP-IDF linker rule that produces this lives in
`components/esp_system/ld/ld.debug.sections`:

```text
.noload 0 (INFO) :
{
    . = 0;
    LONG(0);                                   /* 4-byte NULL reservation */
    _noload_keep_in_elf_start = ABSOLUTE(.);
    SECTION_MAPPINGS(noload_keep_in_elf)
    KEEP(*(.noload_keep_in_elf .noload_keep_in_elf.*))
    _noload_keep_in_elf_end = ABSOLUTE(.);
}
```

Consequences the resolver depends on:

- `0` → output section VMA is 0; the first real format string starts at offset 4
  (after `LONG(0)`).
- `(INFO)` → non-allocatable (`SHF_ALLOC` unset), so it is excluded from the
  flashed binary image and from `PT_LOAD` segments, but **retained in the ELF**.
- `KEEP(...)` → file bytes are not garbage-collected; the section is **PROGBITS**
  with a real file offset, never NOBITS (initialized `const char[]` data is
  PROGBITS by nature; NOBITS is reserved for the `(NOLOAD)` BSS-style sections
  fed by `.bss`-class input).

This is why `elf_resolv.rs` keeps VMA-0 sections whose name contains `.noload`
(and skips all other VMA-0 sections such as `.debug_*` / `.comment`, which the
firmware never points into). `is_noload_section` uses `name.contains(".noload")`;
ESP-IDF also has a `.flash.rodata_noload (NOLOAD)` BSS section, but that is
NOBITS and excluded by the `sh_type == 8` check, and the firmware never emits a
`fmt`/`tag` address into it.

If format strings ever stop resolving (lines render as `<fmt @0x...>`), confirm
the built ELF still matches these assumptions:

```bash
readelf -S <firmware.elf> | grep noload   # expect: .noload at 00000000, PROGBITS, non-zero size
readelf -x .noload <firmware.elf> | head  # expect: format strings as ASCII starting at offset 4
```

If the section name, type, or addressing changes (e.g. a future linker-script
rewrite makes it NOBITS or moves it to a real VMA), the resolver's
`.noload`-name and VMA-0 special-casing must be revisited.

## Known Implementation Risks

The host tool currently passes its Rust unit tests and `cargo clippy -- -D
warnings`, but these review findings are still open:

- **LOG payloads are not fully strict.** `decode_log()` consumes the declared
  argument count and argument bodies, but it does not currently reject extra bytes
  left at the end of the payload. BUFFER and DROPPED packets already check exact
  lengths.
- **Formatter correctness depends on firmware ABI assumptions.** `printf.rs`
  assumes the ESP32 32-bit ABI for default `int`, `long`, `size_t`, pointers, and
  `ptrdiff_t`, and delegates conversion behavior to the `sprintf` crate. It is
  suitable for the current ESP32-S3 target, but it is not a general cross-target
  printf implementation.
- **Unsupported or unusual printf conversions render as errors.** On a
  `sprintf` error the CLI preserves the log by printing `<render error: ...>` plus
  the original format string. This is intentional recovery, not complete printf
  coverage.
- **Decoded on9log messages lose repeated whitespace when wrapped.** `term::wrap`
  splits on whitespace, so multiple spaces/tabs inside decoded messages are
  collapsed. Verified type `0x02` text transport frames are written raw and are
  not affected.
- **Unframed bytes are intentionally ignored.** The host now only trusts
  transport frames beginning with `0xa5`. Early boot output or third-party bytes
  written before the stdio VFS transport is installed will not be displayed
  unless they are wrapped as type `0x02` text frames.

## Output Format

A decoded LOG line looks like:

```text
I (   1234) TAG: rendered message goes here
```

where `I` is the ESP-IDF level letter, the parenthesized number is `time_ms`
since boot, then the tag, then the rendered message. Continuation lines (when
the message wraps) are indented under the message.

With `-t/--timestamp`, decoded log lines and plain-text transport lines are
prefixed with local wall time:

```text
[YYYYmmdd-hh:mm:ss.sss] I (   1234) TAG: rendered message goes here
```

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
- **goblin for ELF.** Resolves `fmt_id`/`tag_id` addresses to C strings. Format
  strings are resolved only from section names containing `.noload`, including
  ESP-IDF's VMA-0 ELF-only no-load output section. Tags are resolved from normal
  non-`.noload` sections. goblin is the user-chosen parser.
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
  ignores bytes outside explicit-start transport frames, and accumulates from a
  starting `0xa5` until a frame-ending `0xc0`, so partial reads and split frames
  are handled naturally. If the opening `0xa5` is missed, the parser hunts until
  the next unescaped `0xa5` and resynchronizes.
- **Lenient recovery.** Bad magic, CRC mismatch, invalid escapes, oversized
  frames, and truncation are reported but do not halt the stream; the next
  unescaped `0xa5` resynchronizes.
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
-t, --timestamp       prefix logs/text lines with local wall time
    --width <WIDTH>   override terminal width (0 = auto-detect)    default 0
```

Tests:

```bash
cargo test       # CRC, sprintf rendering, typed SLIP framing, decode, ELF resolution
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
