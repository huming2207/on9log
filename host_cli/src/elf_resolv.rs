//! Resolve `fmt_id` / `tag_id` ELF addresses back to their C strings.
//!
//! The firmware places format strings in `.noload_keep_in_elf.*` sections and
//! tags in normal read-only sections, then sends only the address. The host
//! opens the matching ELF and maps each address to the NUL-terminated string
//! stored in any section that carries file bytes at that virtual address.

use goblin::elf::{Elf, SectionHeader};

/// Address-indexed ELF string table.
pub struct ElfStrings {
    /// Sections sorted by start address, each carrying its file bytes.
    sections: Vec<Section>,
}

struct Section {
    addr: u32,
    end: u32,
    data: Vec<u8>,
}

impl ElfStrings {
    /// Parse an ELF file from raw bytes (e.g. the Xtensa `.elf` build artifact).
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, String> {
        let elf = Elf::parse(bytes).map_err(|e| format!("elf parse: {e}"))?;

        let mut sections = Vec::new();
        for sh in elf.section_headers.iter() {
            collect_section(bytes, sh, &mut sections);
        }

        // Sort by start address; drop overlaps by preferring earlier entry.
        sections.sort_by_key(|s| s.addr);
        Ok(Self { sections })
    }

    /// Read a NUL-terminated string starting at `addr`, if it falls inside a
    /// known section. Returns `None` for unmapped addresses or empty strings.
    pub fn read_cstr(&self, addr: u32) -> Option<&str> {
        let sec = self.find(addr)?;
        let off = usize::try_from(addr - sec.addr).ok()?;
        let bytes = &sec.data[off..];
        let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
        if end == 0 {
            return None;
        }
        std::str::from_utf8(&bytes[..end]).ok()
    }

    fn find(&self, addr: u32) -> Option<&Section> {
        // Binary search for the last section whose start <= addr.
        let idx = self
            .sections
            .partition_point(|s| s.addr <= addr)
            .checked_sub(1)?;
        let sec = &self.sections[idx];
        if addr < sec.end { Some(sec) } else { None }
    }
}

fn collect_section(file: &[u8], sh: &SectionHeader, out: &mut Vec<Section>) {
    // Only sections with a real virtual address and on-file bytes are useful.
    // NOBITS sections (sh_type == 8) occupy no file space and hold no strings.
    if sh.sh_addr == 0 || sh.sh_size == 0 || sh.sh_type == 8 {
        return;
    }
    let start = usize::try_from(sh.sh_offset).unwrap_or(usize::MAX);
    let size = usize::try_from(sh.sh_size).unwrap_or(usize::MAX);
    let end = start.saturating_add(size);
    if start >= file.len() || end > file.len() {
        return;
    }
    let data = file[start..end].to_vec();
    let addr = u32::try_from(sh.sh_addr).unwrap_or(0);
    let end_addr = addr.saturating_add(u32::try_from(size).unwrap_or(0));
    out.push(Section {
        addr,
        end: end_addr,
        data,
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_file_is_error() {
        assert!(ElfStrings::from_bytes(&[]).is_err());
    }
}
