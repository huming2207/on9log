//! Resolve ELF addresses back to their C strings and symbols.
//!
//! The firmware places format strings in `.noload_keep_in_elf.*` sections and
//! tags in normal read-only sections, then sends only the address. The host
//! opens the matching ELF and maps each address to the NUL-terminated string
//! stored in any section that carries file bytes at that virtual address.

use std::path::Path;

use goblin::{
    elf::{Elf, SectionHeader, sym::STT_FUNC},
    strtab::Strtab,
};

/// Address-indexed ELF string table.
pub struct ElfStrings {
    /// Sections sorted by start address, each carrying its file bytes.
    sections: Vec<Section>,
    /// Function symbols sorted by start address.
    symbols: Vec<Symbol>,
    /// DWARF-backed source location resolver, when this ELF was loaded by path.
    lines: Option<addr2line::Loader>,
}

struct Section {
    name: String,
    addr: u32,
    end: u32,
    data: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResolvedSymbol<'a> {
    pub name: &'a str,
    pub address: u32,
    pub offset: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceLocation {
    pub file: String,
    pub line: Option<u32>,
}

struct Symbol {
    name: String,
    addr: u32,
    size: u32,
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

        let mut symbols = Vec::new();
        collect_symbols(&elf, &mut symbols);

        // Sort by start address; drop overlaps by preferring earlier entry.
        sections.sort_by_key(|s| s.addr);
        symbols.sort_by_key(|s| (s.addr, std::cmp::Reverse(s.size)));
        Ok(Self {
            sections,
            symbols,
            lines: None,
        })
    }

    /// Parse an ELF file from disk and enable DWARF source location lookups.
    pub fn from_path(path: impl AsRef<Path>) -> Result<Self, String> {
        let path = path.as_ref();
        let bytes = std::fs::read(path).map_err(|e| format!("read: {e}"))?;
        let mut elf = Self::from_bytes(&bytes)?;
        elf.lines = addr2line::Loader::new(path).ok();
        Ok(elf)
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

    /// Resolve an instruction address to the nearest containing function
    /// symbol. If the symbol has no size, the next symbol address bounds it.
    pub fn resolve_symbol(&self, addr: u32) -> Option<ResolvedSymbol<'_>> {
        let idx = self.symbols.partition_point(|s| s.addr <= addr);
        for i in (0..idx).rev() {
            let sym = &self.symbols[i];
            let end = if sym.size > 0 {
                sym.addr.saturating_add(sym.size)
            } else {
                self.symbols
                    .iter()
                    .skip(i + 1)
                    .find(|next| next.addr > sym.addr)
                    .map(|next| next.addr)
                    .unwrap_or(u32::MAX)
            };
            if addr < end {
                return Some(ResolvedSymbol {
                    name: &sym.name,
                    address: sym.addr,
                    offset: addr.saturating_sub(sym.addr),
                });
            }
        }
        None
    }

    /// Resolve an instruction address to a DWARF source file and line.
    pub fn resolve_location(&self, addr: u32) -> Option<SourceLocation> {
        let loc = self.lines.as_ref()?.find_location(u64::from(addr)).ok()??;
        let file = loc.file?;
        Some(SourceLocation {
            file: file.to_string(),
            line: loc.line,
        })
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

fn collect_symbols(elf: &Elf<'_>, out: &mut Vec<Symbol>) {
    collect_symbol_table(elf.syms.iter(), &elf.strtab, out);
    collect_symbol_table(elf.dynsyms.iter(), &elf.dynstrtab, out);
}

fn collect_symbol_table(
    symbols: impl Iterator<Item = goblin::elf::Sym>,
    names: &Strtab<'_>,
    out: &mut Vec<Symbol>,
) {
    for sym in symbols {
        if sym.st_value == 0 || sym.st_type() != STT_FUNC {
            continue;
        }
        let Some(name) = names.get_at(sym.st_name).filter(|s| !s.is_empty()) else {
            continue;
        };
        let addr = match u32::try_from(sym.st_value) {
            Ok(addr) => addr,
            Err(_) => continue,
        };
        let size = u32::try_from(sym.st_size).unwrap_or(0);
        if out.iter().any(|s| s.addr == addr && s.name == name) {
            continue;
        }
        out.push(Symbol {
            name: name.to_string(),
            addr,
            size,
        });
    }
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
            symbols: Vec::new(),
            lines: None,
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
            symbols: Vec::new(),
            lines: None,
        };

        assert_eq!(strings.read_format(4), None);
    }

    #[test]
    fn symbol_lookup_uses_containing_function() {
        let strings = ElfStrings {
            sections: Vec::new(),
            symbols: vec![
                Symbol {
                    name: "first".to_string(),
                    addr: 0x1000,
                    size: 0x20,
                },
                Symbol {
                    name: "second".to_string(),
                    addr: 0x1040,
                    size: 0,
                },
            ],
            lines: None,
        };

        assert_eq!(
            strings.resolve_symbol(0x1014),
            Some(ResolvedSymbol {
                name: "first",
                address: 0x1000,
                offset: 0x14,
            })
        );
        assert_eq!(strings.resolve_symbol(0x1030), None);
        assert_eq!(strings.resolve_symbol(0x1044).unwrap().name, "second");
    }
}
