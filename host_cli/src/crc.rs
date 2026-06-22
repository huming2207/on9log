//! CRC-16-CCITT (CCITT-FALSE) matching `on9log_esp_vfs.c`.
//!
//! Initial value `0xffff`, polynomial `0x1021`, no reflection, no final xor.
//! The firmware appends the result little-endian after SLIP-escaping.

const POLY: u16 = 0x1021;

/// Table-driven update identical to the firmware's LUT implementation:
/// `crc = (crc << 8) ^ table[((crc >> 8) ^ byte) & 0xff]`.
fn update(crc: u16, data: &[u8]) -> u16 {
    let mut crc = crc;
    for &b in data {
        let idx = ((crc >> 8) ^ b as u16) & 0xff;
        crc = (crc << 8) ^ table_entry(idx);
    }
    crc
}

#[inline]
fn table_entry(idx: u16) -> u16 {
    let mut crc = idx << 8;
    for _ in 0..8 {
        if crc & 0x8000 != 0 {
            crc = (crc << 1) ^ POLY;
        } else {
            crc <<= 1;
        }
    }
    crc
}

/// Compute CRC-16-CCITT over `header` followed by `payload`, the exact byte
/// span the firmware covers before appending the checksum.
pub fn compute(header: &[u8], payload: &[u8]) -> u16 {
    let mut crc = super::wire::CRC16_CCITT_INIT;
    crc = update(crc, header);
    crc = update(crc, payload);
    crc
}

/// Verify a frame: `crc_bytes` is the little-endian trailing checksum.
pub fn verify(header: &[u8], payload: &[u8], crc_bytes: &[u8; 2]) -> bool {
    let expected = compute(header, payload);
    expected == u16::from_le_bytes(*crc_bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::CRC16_CCITT_INIT;

    // "123456789" with CRC-16/CCITT-FALSE = 0x29B1 (standard check value).
    #[test]
    fn ccitt_false_check_value() {
        let crc = update(CRC16_CCITT_INIT, b"123456789");
        assert_eq!(crc, 0x29b1);
    }
}
