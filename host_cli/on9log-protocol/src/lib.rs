//! Protocol decoder library for on9log binary log streams.
//!
//! This crate is usable both as a library (e.g. via a future `napi-rs` binding)
//! and as the backing for the `on9log` CLI binary. It contains the decoding,
//! framing, crash-text recognition, printf rendering, and ELF symbolication
//! logic, but no UART runtime or terminal presentation code.
//!
//! Typical library usage:
//!
//! ```
//! use on9log_protocol::{Deframer, Decoder, ElfStrings};
//!
//! let mut deframer = Deframer::new();
//! let mut decoder = Decoder::new();
//! let elf = ElfStrings::from_bytes(&[]).ok();
//! let uart_bytes: [u8; 0] = [];
//!
//! for outcome in deframer.feed(&uart_bytes) {
//!     if let on9log_protocol::Outcome::Frame(frame) = outcome {
//!         let _pkt = decoder.decode(&frame, elf.as_ref());
//!     }
//! }
//! ```

pub mod crash;
pub mod cppfmt;
pub mod crc;
pub mod decode;
pub mod elf_resolv;
pub mod framer;
pub mod printf;
pub mod wire;

pub use crash::CrashDecoder;
pub use decode::{BufferRecord, DecodedPacket, Decoder, DroppedRecord, LogRecord, PacketMeta};
pub use elf_resolv::{ElfStrings, ResolvedSymbol, SourceLocation};
pub use framer::{Deframer, Outcome, RawFrame};
pub use printf::{render_format, Arg};
pub use wire::{ArgType, Header, Level, PacketType};
