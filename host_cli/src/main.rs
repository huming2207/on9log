//! `on9log` — host-side CLI for the on9log binary log stream.
//!
//! Opens a UART port, deframes SLIP+CRC packets, decodes them against an
//! optional ELF, and prints colorized, terminal-width-wrapped log lines.

use std::io::Write;
use std::path::PathBuf;
use std::sync::Arc;

use clap::Parser;
use tokio::io::AsyncReadExt;
use tokio_serial::SerialPortBuilderExt;

use on9log_host::{DecodedPacket, Decoder, Deframer, ElfStrings, Level, Outcome, color, term};

/// Host-side decoder for on9log binary log streams.
#[derive(Parser, Debug)]
#[command(name = "on9log", version, about)]
struct Cli {
    /// UART port device path, e.g. /dev/ttyUSB0.
    #[arg(short, long, value_name = "PORT")]
    port: String,

    /// Baud rate.
    #[arg(short, long, value_name = "BAUD", default_value_t = 115_200)]
    baud: u32,

    /// Path to the firmware ELF, used to resolve format/tag string addresses.
    #[arg(long, value_name = "FILE")]
    elf: Option<PathBuf>,

    /// Disable colored output.
    #[arg(long)]
    no_color: bool,

    /// Override detected terminal width (0 = auto-detect).
    #[arg(long, default_value_t = 0)]
    width: usize,
}

fn level_color(level: Level) -> &'static str {
    match level {
        Level::Error => color::RED,
        Level::Warn => color::YELLOW,
        Level::Info => color::GREEN,
        _ => color::WHITE,
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();

    let use_color = !cli.no_color && term::stdout_is_tty();
    let width = if cli.width > 0 {
        cli.width
    } else {
        term::terminal_width()
    };

    let elf = match &cli.elf {
        Some(p) => {
            let bytes = std::fs::read(p)?;
            match ElfStrings::from_bytes(&bytes) {
                Ok(e) => {
                    eprintln!("on9log: loaded ELF {}", p.display());
                    Some(Arc::new(e))
                }
                Err(e) => {
                    eprintln!("on9log: failed to parse ELF {}: {e}", p.display());
                    None
                }
            }
        }
        None => {
            eprintln!("on9log: no --elf given; format/tag addresses will render as hex");
            None
        }
    };

    let rt = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()?;
    rt.block_on(run(cli.port, cli.baud, elf, use_color, width))?;
    Ok(())
}

async fn run(
    port: String,
    baud: u32,
    elf: Option<Arc<ElfStrings>>,
    use_color: bool,
    width: usize,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut serial = tokio_serial::new(&port, baud)
        .open_native_async()
        .map_err(|e| format!("opening {port}: {e}"))?;

    let mut deframer = Deframer::new();
    let mut decoder = Decoder::new();
    let mut buf = vec![0u8; 4096];

    eprintln!("on9log: listening on {port} @ {baud} baud (width {width})");

    loop {
        let n = serial.read(&mut buf).await?;
        if n == 0 {
            // EOF on the device; stop.
            break;
        }
        for outcome in deframer.feed(&buf[..n]) {
            handle_outcome(outcome, &mut decoder, elf.as_deref(), use_color, width);
        }
    }

    Ok(())
}

fn handle_outcome(
    outcome: Outcome,
    decoder: &mut Decoder,
    elf: Option<&ElfStrings>,
    use_color: bool,
    width: usize,
) {
    match outcome {
        Outcome::Frame(frame) => match decoder.decode(&frame, elf) {
            DecodedPacket::Log(l) => {
                let color_code = level_color(l.level);
                let prefix = format!("{} ({:>7}) {}: ", l.level.letter(), l.meta.time_ms, l.tag);
                let indent = prefix.chars().count();
                if let Some(gap) = l.meta.gap {
                    warn_line(
                        &format!("--- missed {gap} packet(s) before seq {} ---", l.meta.seq),
                        use_color,
                    );
                }
                term::print_log_line(&prefix, &l.message, color_code, indent, width, use_color);
            }
            DecodedPacket::Dropped(d) => {
                warn_line(
                    &format!(
                        "--- device dropped {} packet(s) at seq {} (t={}ms) ---",
                        d.count, d.meta.seq, d.meta.time_ms
                    ),
                    use_color,
                );
            }
            DecodedPacket::Buffer(b) => {
                let color_code = level_color(b.level);
                let header = format!(
                    "{} ({:>7}) {} [buf {} @{} +{}]: <buffer dump, {} bytes>",
                    b.level.letter(),
                    b.meta.time_ms,
                    b.tag,
                    b.total_len,
                    b.offset,
                    b.bytes.len(),
                    b.bytes.len()
                );
                if use_color {
                    println!(
                        "{color}{BOLD}{header}{RESET}",
                        color = color_code,
                        BOLD = color::BOLD,
                        RESET = color::RESET
                    );
                } else {
                    println!("{header}");
                }
                print_hexdump(&b.bytes, color_code, use_color);
            }
            DecodedPacket::Other {
                meta,
                kind,
                payload,
                ..
            } => {
                eprintln!(
                    "on9log: {kind} packet seq {} t={}ms ({} bytes)",
                    meta.seq,
                    meta.time_ms,
                    payload.len()
                );
            }
            DecodedPacket::Malformed { meta, reason } => {
                let seq = meta.as_ref().map(|m| m.seq.to_string()).unwrap_or_default();
                eprintln!("on9log: malformed packet seq {seq}: {reason}");
            }
        },
        Outcome::BadMagic => eprintln!("on9log: frame with bad magic discarded"),
        Outcome::CrcMismatch => eprintln!("on9log: frame failed CRC, discarded"),
        Outcome::Truncated => eprintln!("on9log: truncated frame discarded"),
        Outcome::LengthMismatch => eprintln!("on9log: frame payload length mismatch, discarded"),
        Outcome::PlainText(bytes) => {
            let mut out = std::io::stdout().lock();
            let _ = out.write_all(&bytes);
            let _ = out.flush();
        }
    }
}

fn warn_line(msg: &str, use_color: bool) {
    if use_color {
        println!(
            "{DIM}{YELLOW}{msg}{RESET}",
            DIM = color::DIM,
            YELLOW = color::YELLOW,
            RESET = color::RESET
        );
    } else {
        println!("{msg}");
    }
}

fn print_hexdump(bytes: &[u8], color_code: &str, use_color: bool) {
    const BYTES_PER_LINE: usize = 16;
    for (i, chunk) in bytes.chunks(BYTES_PER_LINE).enumerate() {
        let addr = (i * BYTES_PER_LINE) as u32;
        let hex: Vec<String> = chunk.iter().map(|b| format!("{:02x}", b)).collect();
        let ascii: String = chunk
            .iter()
            .map(|&b| {
                if (0x20..0x7f).contains(&b) {
                    b as char
                } else {
                    '.'
                }
            })
            .collect();
        let line = format!("{:08x}  {:<48}  {}", addr, hex.join(" "), ascii);
        if use_color {
            println!(
                "{color}{line}{RESET}",
                color = color_code,
                RESET = color::RESET
            );
        } else {
            println!("{line}");
        }
    }
}
