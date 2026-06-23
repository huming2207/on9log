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
    name: String,
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
            let name = elf.shdr_strtab.get_at(sh.sh_name).unwrap_or_default();
            collect_section(bytes, sh, name, &mut sections);
        }

        // Sort by start address; drop overlaps by preferring earlier entry.
        sections.sort_by_key(|s| s.addr);
        Ok(Self { sections })
    }

    /// Read a NUL-terminated string starting at `addr`, if it falls inside a
    /// known section. Returns `None` for unmapped addresses or empty strings.
    pub fn read_cstr(&self, addr: u32) -> Option<&str> {
        self.read_cstr_from(addr, |_| true)
    }

    /// Read a format string. Format strings are expected to live in ESP-IDF's
    /// ELF-only no-load section family.
    pub fn read_format(&self, addr: u32) -> Option<&str> {
        self.read_cstr_from(addr, is_noload_section)
    }

    /// Read a normal tag string. Tags are expected in ordinary string-bearing
    /// sections, not the no-load format-string section.
    pub fn read_tag(&self, addr: u32) -> Option<&str> {
        self.read_cstr_from(addr, |name| !is_noload_section(name))
    }

    fn read_cstr_from<P>(&self, addr: u32, section_matches: P) -> Option<&str>
    where
        P: Fn(&str) -> bool,
    {
        let mut found: Option<&str> = None;
        for sec in self
            .sections
            .iter()
            .filter(|s| section_matches(&s.name) && s.contains(addr))
        {
            let s = sec.read_cstr(addr)?;
            match found {
                Some(prev) if prev != s => return None,
                Some(_) => {}
                None => found = Some(s),
            }
        }
        found
    }
}

impl Section {
    fn contains(&self, addr: u32) -> bool {
        self.addr <= addr && addr < self.end
    }

    fn read_cstr(&self, addr: u32) -> Option<&str> {
        let off = usize::try_from(addr - self.addr).ok()?;
        let bytes = self.data.get(off..)?;
        let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
        if end == 0 {
            return None;
        }
        std::str::from_utf8(&bytes[..end]).ok()
    }
}

fn is_noload_section(name: &str) -> bool {
    name.contains(".noload")
}

fn collect_section(file: &[u8], sh: &SectionHeader, name: &str, out: &mut Vec<Section>) {
    // NOBITS sections occupy no file space and hold no strings. ESP-IDF's
    // no-load strings are PROGBITS at VMA 0, so keep VMA-0 sections only when
    // their output section name identifies them as no-load.
    if sh.sh_size == 0 || sh.sh_type == 8 {
        return;
    }
    if sh.sh_addr == 0 && !is_noload_section(name) {
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
        name: name.to_string(),
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

    #[test]
    fn format_lookup_uses_noload_section_at_vma_zero() {
        let strings = ElfStrings {
            sections: vec![
                Section {
                    name: ".rodata".to_string(),
                    addr: 0x3f00_0000,
                    end: 0x3f00_0010,
                    data: b"TAG\0".to_vec(),
                },
                Section {
                    name: ".noload".to_string(),
                    addr: 0,
                    end: 16,
                    data: [0, 0, 0, 0, b'f', b'm', b't', b'\0'].to_vec(),
                },
            ],
        };

        assert_eq!(strings.read_format(4), Some("fmt"));
        assert_eq!(strings.read_tag(0x3f00_0000), Some("TAG"));
        assert_eq!(strings.read_tag(4), None);
    }

    #[test]
    fn ambiguous_lookup_with_different_strings_fails() {
        let strings = ElfStrings {
            sections: vec![
                Section {
                    name: ".noload".to_string(),
                    addr: 0,
                    end: 8,
                    data: b"\0\0\0\0a\0".to_vec(),
                },
                Section {
                    name: ".noload.extra".to_string(),
                    addr: 4,
                    end: 8,
                    data: b"b\0".to_vec(),
                },
            ],
        };

        assert_eq!(strings.read_format(4), None);
    }
}
