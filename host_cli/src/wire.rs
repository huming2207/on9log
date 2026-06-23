//! on9log wire format: packet header, packet types, log levels, argument types.
//!
//! Mirrors `on9log_fmt.h` and the `ON9_LOG_ARGS_TYPE_*` values from `on9log.h`.
//! All multi-byte fields are little-endian.

pub const PACKET_MAGIC: u8 = 0x9a;
pub const PAYLOAD_LEN_STREAMING: u16 = 0xffff;
pub const HEADER_LEN: usize = 18;
pub const NULL_STRING_LEN: u32 = 0xffff_ffff;
pub const TRANSPORT_MAX_PAYLOAD: usize = 3 * 1024;
pub const TRANSPORT_FRAME_ON9LOG: u8 = 0x01;
pub const TRANSPORT_FRAME_TEXT: u8 = 0x02;

/// CRC-16-CCITT (CCITT-FALSE) initial value, matching `esp_stdio_log_vfs.c`.
pub const CRC16_CCITT_INIT: u16 = 0xffff;

/// Transport framing bytes from `esp_stdio_log_vfs.c`.
pub const SLIP_START: u8 = 0xa5;
pub const SLIP_END: u8 = 0xc0;
pub const SLIP_ESC: u8 = 0xdb;
pub const SLIP_ESC_END: u8 = 0xdc;
pub const SLIP_ESC_ESC: u8 = 0xdd;
pub const SLIP_ESC_START: u8 = 0xde;

/// Packet type, stored in the high nibble of `type_level`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum PacketType {
    Log = 0,
    Dropped = 1,
    TimeSync = 2,
    Boot = 3,
    Buffer = 4,
}

impl PacketType {
    pub fn from_byte(b: u8) -> Option<Self> {
        match b {
            0 => Some(Self::Log),
            1 => Some(Self::Dropped),
            2 => Some(Self::TimeSync),
            3 => Some(Self::Boot),
            4 => Some(Self::Buffer),
            _ => None,
        }
    }
}

/// `on9log_level_t` values, stored in the low nibble of `type_level`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Level {
    None = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4,
    Verbose = 5,
}

impl Level {
    pub fn from_byte(b: u8) -> Option<Self> {
        match b {
            0 => Some(Self::None),
            1 => Some(Self::Error),
            2 => Some(Self::Warn),
            3 => Some(Self::Info),
            4 => Some(Self::Debug),
            5 => Some(Self::Verbose),
            _ => None,
        }
    }

    /// Single-character ESP-IDF-style level tag (e.g. `I` for INFO).
    pub fn letter(self) -> char {
        match self {
            Self::None => 'N',
            Self::Error => 'E',
            Self::Warn => 'W',
            Self::Info => 'I',
            Self::Debug => 'D',
            Self::Verbose => 'V',
        }
    }
}

/// Argument type values from `ON9_LOG_ARGS_TYPE_*`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ArgType {
    None = 0,
    Bits32 = 1,
    Bits64 = 2,
    Pointer = 3,
    DynamicString = 4,
}

impl ArgType {
    pub fn from_byte(b: u8) -> Option<Self> {
        match b {
            0 => Some(Self::None),
            1 => Some(Self::Bits32),
            2 => Some(Self::Bits64),
            3 => Some(Self::Pointer),
            4 => Some(Self::DynamicString),
            _ => None,
        }
    }
}

/// Parsed 18-byte packet header.
#[derive(Debug, Clone, Copy)]
pub struct Header {
    pub magic: u8,
    pub ptype: PacketType,
    pub level: Level,
    pub seq: u16,
    pub time_ms: u32,
    pub tag_id: u32,
    pub fmt_id: u32,
    pub payload_len: u16,
}

impl Header {
    /// Parse an 18-byte little-endian header. Returns `None` on magic mismatch or
    /// unknown type/level nibbles.
    pub fn parse(buf: &[u8]) -> Option<Self> {
        if buf.len() < HEADER_LEN {
            return None;
        }
        let magic = buf[0];
        if magic != PACKET_MAGIC {
            return None;
        }
        let type_level = buf[1];
        let ptype = PacketType::from_byte(type_level >> 4)?;
        let level = Level::from_byte(type_level & 0x0f)?;
        let seq = u16::from_le_bytes([buf[2], buf[3]]);
        let time_ms = u32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]);
        let tag_id = u32::from_le_bytes([buf[8], buf[9], buf[10], buf[11]]);
        let fmt_id = u32::from_le_bytes([buf[12], buf[13], buf[14], buf[15]]);
        // payload_len occupies the final two bytes of the packed 18-byte header.
        let payload_len = u16::from_le_bytes([buf[16], buf[17]]);
        Some(Self {
            magic,
            ptype,
            level,
            seq,
            time_ms,
            tag_id,
            fmt_id,
            payload_len,
        })
    }
}
