//! SLIP deframing + CRC verification.
//!
//! The ESP VFS sink wraps every packet as:
//! `0xc0 SLIP(header) SLIP(payload) SLIP(crc16_le) 0xc0`.
//! This module turns the raw UART byte stream into verified frames.

use crate::crc;
use crate::wire::{
    HEADER_LEN, Header, PAYLOAD_LEN_STREAMING, SLIP_END, SLIP_ESC, SLIP_ESC_END, SLIP_ESC_ESC,
};

/// A fully decoded, CRC-verified raw frame: its 18-byte header bytes and the
/// payload (header and payload are what the CRC covers).
#[derive(Debug, Clone)]
pub struct RawFrame {
    pub header: Header,
    pub header_bytes: Vec<u8>,
    pub payload: Vec<u8>,
}

/// Outcome of feeding bytes: either a verified frame or a recoverable error
/// (bad magic, CRC mismatch, truncated). Errors do not halt decoding.
#[derive(Debug)]
pub enum Outcome {
    Frame(RawFrame),
    PlainText(Vec<u8>),
    BadMagic,
    CrcMismatch,
    Truncated,
    LengthMismatch,
}

/// Streaming SLIP deframer. Feed it arbitrary byte slices from the transport.
pub struct Deframer {
    buf: Vec<u8>,
    plain: Vec<u8>,
    in_frame: bool,
    escape: bool,
}

impl Default for Deframer {
    fn default() -> Self {
        Self::new()
    }
}

impl Deframer {
    pub fn new() -> Self {
        Self {
            buf: Vec::with_capacity(256),
            plain: Vec::with_capacity(256),
            in_frame: false,
            escape: false,
        }
    }

    /// Feed a chunk of transport bytes; returns completed frame outcomes in
    /// arrival order.
    pub fn feed(&mut self, data: &[u8]) -> Vec<Outcome> {
        let mut out = Vec::new();
        for &b in data {
            if !self.in_frame {
                if b == SLIP_END {
                    if !self.plain.is_empty() {
                        out.push(Outcome::PlainText(std::mem::take(&mut self.plain)));
                    }
                    self.in_frame = true;
                    self.escape = false;
                    self.buf.clear();
                } else {
                    self.plain.push(b);
                }
                continue;
            }

            match b {
                SLIP_END => {
                    if self.buf.is_empty() {
                        self.escape = false;
                        continue;
                    }
                    if let Some(o) = Self::decode_frame(&self.buf) {
                        out.push(o);
                    }
                    self.buf.clear();
                    self.escape = false;
                    self.in_frame = false;
                }
                SLIP_ESC => self.escape = true,
                other if self.escape => {
                    let unescaped = match other {
                        SLIP_ESC_END => Some(SLIP_END),
                        SLIP_ESC_ESC => Some(SLIP_ESC),
                        _ => None, // invalid escape: drop this byte, stay lenient
                    };
                    if let Some(v) = unescaped {
                        self.buf.push(v);
                    }
                    self.escape = false;
                }
                other => {
                    self.buf.push(other);
                }
            }
        }
        out
    }

    /// Decode one unescaped frame buffer into an `Outcome`.
    fn decode_frame(buf: &[u8]) -> Option<Outcome> {
        // header(18) + crc(2) is the minimum.
        if buf.len() < HEADER_LEN + 2 {
            return Some(Outcome::Truncated);
        }
        let header = match Header::parse(&buf[..HEADER_LEN]) {
            Some(h) => h,
            None => return Some(Outcome::BadMagic),
        };
        let crc_bytes: [u8; 2] = [buf[buf.len() - 2], buf[buf.len() - 1]];
        // Bytes covered by the CRC: header + payload (everything except the CRC).
        let covered = &buf[..buf.len() - 2];
        let payload = &covered[HEADER_LEN..];

        if !crc::verify(&covered[..HEADER_LEN], payload, &crc_bytes) {
            return Some(Outcome::CrcMismatch);
        }
        if header.payload_len != PAYLOAD_LEN_STREAMING
            && payload.len() != usize::from(header.payload_len)
        {
            return Some(Outcome::LengthMismatch);
        }

        Some(Outcome::Frame(RawFrame {
            header,
            header_bytes: buf[..HEADER_LEN].to_vec(),
            payload: payload.to_vec(),
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::{
        ArgType, HEADER_LEN, Level, PACKET_MAGIC, PAYLOAD_LEN_STREAMING, PacketType,
    };

    /// Build a full on9log SLIP frame exactly as `on9log_esp_vfs.c` would emit
    /// it: `0xc0 SLIP(header) SLIP(payload) SLIP(crc_le) 0xc0`.
    fn build_frame(header: &[u8], payload: &[u8]) -> Vec<u8> {
        let crc = crate::crc::compute(header, payload);
        let mut inner = Vec::new();
        inner.extend_from_slice(header);
        inner.extend_from_slice(payload);
        inner.extend_from_slice(&crc.to_le_bytes());

        let mut out = vec![SLIP_END];
        for &b in &inner {
            match b {
                SLIP_END => {
                    out.push(SLIP_ESC);
                    out.push(SLIP_ESC_END);
                }
                SLIP_ESC => {
                    out.push(SLIP_ESC);
                    out.push(SLIP_ESC_ESC);
                }
                other => out.push(other),
            }
        }
        out.push(SLIP_END);
        out
    }

    fn make_header(seq: u16) -> Vec<u8> {
        let mut h = Vec::with_capacity(HEADER_LEN);
        h.push(PACKET_MAGIC);
        h.push(((PacketType::Log as u8) << 4) | (Level::Info as u8));
        h.extend_from_slice(&seq.to_le_bytes());
        h.extend_from_slice(&1000u32.to_le_bytes()); // time_ms
        h.extend_from_slice(&0x4000_0000u32.to_le_bytes()); // tag_id
        h.extend_from_slice(&0x4000_1000u32.to_le_bytes()); // fmt_id
        h.extend_from_slice(&PAYLOAD_LEN_STREAMING.to_le_bytes());
        h
    }

    #[test]
    fn deframes_full_slip_frame() {
        // payload: one u32 arg = 42
        let mut payload = vec![1u8, ArgType::Bits32 as u8];
        payload.extend_from_slice(&42u32.to_le_bytes());

        let header = make_header(7);
        let wire = build_frame(&header, &payload);

        let mut d = Deframer::new();
        let outcomes = d.feed(&wire);
        assert_eq!(outcomes.len(), 1);
        match &outcomes[0] {
            Outcome::Frame(f) => {
                assert_eq!(f.header.seq, 7);
                assert_eq!(f.header.level, Level::Info);
                assert_eq!(f.header.ptype, PacketType::Log);
                assert_eq!(f.payload, payload);
            }
            o => panic!("expected Frame, got {o:?}"),
        }
    }

    #[test]
    fn detects_crc_corruption() {
        let payload = vec![0u8]; // arg_count 0
        let header = make_header(1);
        let mut wire = build_frame(&header, &payload);
        // Flip a payload byte (index: END + header_len).
        let idx = 1 + HEADER_LEN;
        wire[idx] ^= 0xff;
        let mut d = Deframer::new();
        let outcomes = d.feed(&wire);
        assert!(matches!(outcomes[0], Outcome::CrcMismatch));
    }

    #[test]
    fn rejects_non_streaming_length_mismatch() {
        let mut payload = 1u32.to_le_bytes().to_vec();
        payload.push(0xff);
        let mut header = make_header(3);
        let len_offset = HEADER_LEN - 2;
        header[len_offset..HEADER_LEN].copy_from_slice(&4u16.to_le_bytes());
        let wire = build_frame(&header, &payload);

        let mut d = Deframer::new();
        let outcomes = d.feed(&wire);
        assert!(matches!(outcomes[0], Outcome::LengthMismatch));
    }

    #[test]
    fn handles_split_feeds_and_escape() {
        // Payload containing 0xc0 bytes to exercise SLIP escaping.
        let mut payload = vec![1u8, ArgType::Bits32 as u8];
        payload.extend_from_slice(&0xc0_c0_c0_c0u32.to_le_bytes());
        let header = make_header(2);
        let wire = build_frame(&header, &payload);

        // Feed in two chunks to verify incremental deframing.
        let split = wire.len() / 2;
        let mut d = Deframer::new();
        let mut outcomes = d.feed(&wire[..split]);
        outcomes.extend(d.feed(&wire[split..]));
        assert_eq!(outcomes.len(), 1);
        match &outcomes[0] {
            Outcome::Frame(f) => assert_eq!(f.payload, payload),
            o => panic!("expected Frame, got {o:?}"),
        }
    }

    #[test]
    fn emits_plain_text_before_slip_frame() {
        let payload = vec![0u8];
        let header = make_header(4);
        let mut wire = b"I (123) boot: hello\n".to_vec();
        wire.extend(build_frame(&header, &payload));

        let mut d = Deframer::new();
        let outcomes = d.feed(&wire);
        assert_eq!(outcomes.len(), 2);
        match &outcomes[0] {
            Outcome::PlainText(text) => assert_eq!(text, b"I (123) boot: hello\n"),
            o => panic!("expected PlainText, got {o:?}"),
        }
        assert!(matches!(outcomes[1], Outcome::Frame(_)));
    }

    #[test]
    fn preserves_plain_text_across_split_feeds() {
        let mut d = Deframer::new();
        assert!(d.feed(b"I (").is_empty());
        let outcomes = d.feed(b"1) tag: msg\n\xc0");
        assert_eq!(outcomes.len(), 1);
        match &outcomes[0] {
            Outcome::PlainText(text) => assert_eq!(text, b"I (1) tag: msg\n"),
            o => panic!("expected PlainText, got {o:?}"),
        }
    }
}
