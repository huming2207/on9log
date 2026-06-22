//! Host-side decoder library for on9log binary log streams.
//!
//! This crate is usable both as a library (e.g. via a future `napi-rs` binding)
//! and as the backing for the `on9log` CLI binary. The pure-decoding core
//! (`wire`, `crc`, `framer`, `elf_resolv`, `printf`, `decode`) has no runtime
//! dependency; only the CLI binary pulls in `tokio` / `tokio-serial`.
//!
//! Typical library usage:
//!
//! ```
//! use on9log_host::{Deframer, Decoder, ElfStrings};
//!
//! let mut deframer = Deframer::new();
//! let mut decoder = Decoder::new();
//! let elf = ElfStrings::from_bytes(&[]).ok();
//! let uart_bytes: [u8; 0] = [];
//!
//! for outcome in deframer.feed(&uart_bytes) {
//!     if let on9log_host::Outcome::Frame(frame) = outcome {
//!         let _pkt = decoder.decode(&frame, elf.as_ref());
//!     }
//! }
//! ```

pub mod crc;
pub mod decode;
pub mod elf_resolv;
pub mod framer;
pub mod printf;
pub mod term;
pub mod wire;

pub use decode::{BufferRecord, DecodedPacket, Decoder, DroppedRecord, LogRecord, PacketMeta};
pub use elf_resolv::ElfStrings;
pub use framer::{Deframer, Outcome, RawFrame};
pub use printf::Arg;
pub use term::color;
pub use wire::{ArgType, Header, Level, PacketType};
