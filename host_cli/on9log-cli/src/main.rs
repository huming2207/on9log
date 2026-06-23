//! `on9log` — host-side CLI for the on9log binary log stream.
//!
//! Opens a UART port, deframes typed SLIP+CRC transport frames, decodes on9log
//! packets against an optional ELF, and prints colorized log lines.

use std::io::Write;
use std::path::PathBuf;
use std::ptr;
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use clap::Parser;
use tokio::io::AsyncReadExt;
use tokio_serial::{SerialPort, SerialPortBuilderExt};

use on9log_protocol::{CrashDecoder, DecodedPacket, Decoder, Deframer, ElfStrings, Level, Outcome};

mod term;

use term::color;

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

    /// Prefix each decoded log and each plain-text line with local wall time.
    #[arg(short = 't', long)]
    timestamp: bool,

    /// Override detected terminal width (0 = auto-detect).
    #[arg(long, default_value_t = 0)]
    width: usize,

    /// Do not reset ESP targets by toggling DTR/RTS when the port opens.
    #[arg(long)]
    no_esp_reset: bool,

    /// Save decoded human-readable log output to a text file.
    #[arg(short = 's', long)]
    save: bool,

    /// Path for --save output; defaults to on9log-UNIX_TIMESTAMP.log.
    #[arg(long, value_name = "FILE", requires = "save")]
    log_path: Option<PathBuf>,
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
        Some(p) => match ElfStrings::from_path(p) {
            Ok(e) => {
                eprintln!("on9log: loaded ELF {}", p.display());
                Some(Arc::new(e))
            }
            Err(e) => {
                eprintln!("on9log: failed to parse ELF {}: {e}", p.display());
                None
            }
        },
        None => {
            eprintln!("on9log: no --elf given; format/tag addresses will render as hex");
            None
        }
    };
    let save = if cli.save {
        Some(SaveLog::create(cli.log_path.clone())?)
    } else {
        None
    };

    let rt = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()?;
    rt.block_on(run(
        cli.port,
        cli.baud,
        elf,
        use_color,
        width,
        cli.timestamp,
        !cli.no_esp_reset,
        save,
    ))?;
    Ok(())
}

async fn run(
    port: String,
    baud: u32,
    elf: Option<Arc<ElfStrings>>,
    use_color: bool,
    width: usize,
    timestamp: bool,
    esp_reset: bool,
    mut save: Option<SaveLog>,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut serial = tokio_serial::new(&port, baud)
        .open_native_async()
        .map_err(|e| format!("opening {port}: {e}"))?;

    if esp_reset {
        esp_hard_reset(&mut serial).map_err(|e| format!("resetting ESP target on {port}: {e}"))?;
    }

    let mut deframer = Deframer::new();
    let mut decoder = Decoder::new();
    let mut crash_decoder = CrashDecoder::new();
    let mut buf = vec![0u8; 4096];
    let mut stdin_buf = [0u8; 64];
    let mut plain_text = PlainTextState::new();
    let mut save_plain_text = PlainTextState::new();
    let mut monitor_keys = MonitorKeyState::new();
    let mut stdin = tokio::io::stdin();
    let raw_input = RawInputGuard::enter_if_interactive();

    eprintln!(
        "on9log: listening on {port} @ {baud} baud (width {width}, esp-reset {}, quit Ctrl+] Ctrl+T)",
        if esp_reset { "on" } else { "off" }
    );
    if let Some(save) = save.as_ref() {
        eprintln!("on9log: saving decoded log to {}", save.path.display());
    }

    loop {
        let n = if raw_input.is_active() {
            tokio::select! {
                serial_read = serial.read(&mut buf) => serial_read?,
                stdin_read = stdin.read(&mut stdin_buf) => {
                    let n = stdin_read?;
                    if n == 0 {
                        continue;
                    }
                    if monitor_keys.feed(&stdin_buf[..n]) {
                        eprintln!("on9log: quit requested");
                        break;
                    }
                    continue;
                }
            }
        } else {
            serial.read(&mut buf).await?
        };
        if n == 0 {
            // EOF on the device; stop.
            break;
        }
        for outcome in deframer.feed(&buf[..n]) {
            handle_outcome(
                outcome,
                &mut decoder,
                elf.as_deref(),
                use_color,
                width,
                timestamp,
                &mut plain_text,
                &mut crash_decoder,
                save.as_mut(),
                &mut save_plain_text,
            );
        }
    }

    Ok(())
}

struct SaveLog {
    path: PathBuf,
    file: std::fs::File,
}

impl SaveLog {
    fn create(path: Option<PathBuf>) -> std::io::Result<Self> {
        let path = path.unwrap_or_else(default_save_path);
        let file = std::fs::File::create(&path)?;
        Ok(Self { path, file })
    }

    fn write_all_immediate(&mut self, bytes: &[u8]) -> std::io::Result<()> {
        self.file.write_all(bytes)?;
        self.file.flush()?;
        self.file.sync_data()
    }

    fn write_line(&mut self, line: &str) -> std::io::Result<()> {
        self.file.write_all(line.as_bytes())?;
        self.file.write_all(b"\n")?;
        self.file.flush()?;
        self.file.sync_data()
    }
}

fn default_save_path() -> PathBuf {
    let ts = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    PathBuf::from(format!("on9log-{ts}.log"))
}

struct RawInputGuard {
    active: bool,
    #[cfg(unix)]
    fd: std::os::fd::RawFd,
    #[cfg(unix)]
    original: libc::termios,
}

impl RawInputGuard {
    fn enter_if_interactive() -> Self {
        if !term::stdin_is_tty() {
            return Self::inactive();
        }
        match enable_raw_input_mode() {
            Ok(guard) => guard,
            Err(e) => {
                eprintln!("on9log: failed to enable terminal control keys: {e}");
                Self::inactive()
            }
        }
    }

    fn inactive() -> Self {
        Self {
            active: false,
            #[cfg(unix)]
            fd: -1,
            #[cfg(unix)]
            original: unsafe { std::mem::zeroed() },
        }
    }

    fn is_active(&self) -> bool {
        self.active
    }
}

impl Drop for RawInputGuard {
    fn drop(&mut self) {
        #[cfg(unix)]
        if self.active {
            unsafe {
                libc::tcsetattr(self.fd, libc::TCSANOW, &self.original);
            }
        }
        #[cfg(not(unix))]
        if self.active {
            let _ = crossterm::terminal::disable_raw_mode();
        }
    }
}

#[cfg(unix)]
fn enable_raw_input_mode() -> std::io::Result<RawInputGuard> {
    use std::os::fd::AsRawFd;

    let fd = std::io::stdin().as_raw_fd();
    let mut original = std::mem::MaybeUninit::<libc::termios>::uninit();
    if unsafe { libc::tcgetattr(fd, original.as_mut_ptr()) } != 0 {
        return Err(std::io::Error::last_os_error());
    }
    let original = unsafe { original.assume_init() };
    let mut raw = original;

    // Non-canonical input lets Ctrl+] Ctrl+T arrive immediately. Keep output
    // processing intact so monitor newlines are not affected.
    raw.c_lflag &= !(libc::ICANON | libc::ECHO | libc::ISIG | libc::IEXTEN);
    raw.c_cc[libc::VMIN] = 1;
    raw.c_cc[libc::VTIME] = 0;

    if unsafe { libc::tcsetattr(fd, libc::TCSANOW, &raw) } != 0 {
        return Err(std::io::Error::last_os_error());
    }

    Ok(RawInputGuard {
        active: true,
        fd,
        original,
    })
}

#[cfg(not(unix))]
fn enable_raw_input_mode() -> std::io::Result<RawInputGuard> {
    crossterm::terminal::enable_raw_mode()?;
    Ok(RawInputGuard { active: true })
}

struct MonitorKeyState {
    saw_ctrl_rbracket: bool,
}

impl MonitorKeyState {
    fn new() -> Self {
        Self {
            saw_ctrl_rbracket: false,
        }
    }

    fn feed(&mut self, bytes: &[u8]) -> bool {
        for &b in bytes {
            match (self.saw_ctrl_rbracket, b) {
                (true, CTRL_T) => return true,
                (_, CTRL_RBRACKET) => self.saw_ctrl_rbracket = true,
                _ => self.saw_ctrl_rbracket = false,
            }
        }
        false
    }
}

const CTRL_RBRACKET: u8 = 0x1d;
const CTRL_T: u8 = 0x14;

fn esp_hard_reset<P: SerialPort>(port: &mut P) -> tokio_serial::Result<()> {
    // ESP dev boards wire DTR to GPIO0 and RTS to EN through active-low
    // transistor circuits. Release GPIO0, pulse EN low, then release EN.
    port.write_data_terminal_ready(false)?;
    port.write_request_to_send(true)?;
    std::thread::sleep(Duration::from_millis(100));
    port.write_request_to_send(false)?;
    std::thread::sleep(Duration::from_millis(100));
    Ok(())
}

fn handle_outcome(
    outcome: Outcome,
    decoder: &mut Decoder,
    elf: Option<&ElfStrings>,
    use_color: bool,
    width: usize,
    timestamp: bool,
    plain_text: &mut PlainTextState,
    crash_decoder: &mut CrashDecoder,
    mut save: Option<&mut SaveLog>,
    save_plain_text: &mut PlainTextState,
) {
    match outcome {
        Outcome::Frame(frame) => match decoder.decode(&frame, elf) {
            DecodedPacket::Log(l) => {
                let color_code = level_color(l.level);
                let prefix = format!(
                    "{}{} ({:>7}) {}: ",
                    timestamp_prefix(timestamp),
                    l.level.letter(),
                    l.meta.time_ms,
                    l.tag
                );
                let indent = prefix.chars().count();
                if let Some(gap) = l.meta.gap {
                    let msg = format!("--- missed {gap} packet(s) before seq {} ---", l.meta.seq);
                    warn_line(&msg, use_color, timestamp);
                    if let Some(save) = save.as_deref_mut() {
                        let _ = save.write_line(&format!("{}{}", timestamp_prefix(timestamp), msg));
                    }
                }
                term::print_log_line(&prefix, &l.message, color_code, indent, width, use_color);
                if let Some(save) = save.as_deref_mut() {
                    let _ = save.write_line(&format!("{prefix}{}", l.message));
                }
                plain_text.reset_line();
                save_plain_text.reset_line();
            }
            DecodedPacket::Dropped(d) => {
                let msg = format!(
                    "--- device dropped {} packet(s) at seq {} (t={}ms) ---",
                    d.count, d.meta.seq, d.meta.time_ms
                );
                warn_line(&msg, use_color, timestamp);
                if let Some(save) = save.as_deref_mut() {
                    let _ = save.write_line(&format!("{}{}", timestamp_prefix(timestamp), msg));
                }
                plain_text.reset_line();
                save_plain_text.reset_line();
            }
            DecodedPacket::Buffer(b) => {
                let color_code = level_color(b.level);
                let header = format!(
                    "{}{} ({:>7}) {} [buf {} @{} +{}]: <buffer dump, {} bytes>",
                    timestamp_prefix(timestamp),
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
                if let Some(save) = save.as_deref_mut() {
                    let _ = save.write_line(&header);
                }
                print_hexdump(&b.bytes, color_code, use_color, timestamp);
                if let Some(save) = save.as_deref_mut() {
                    let _ = write_hexdump_to_file(save, &b.bytes, timestamp);
                }
                plain_text.reset_line();
                save_plain_text.reset_line();
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
        Outcome::FrameTooLong => {
            eprintln!("on9log: transport frame exceeded maximum length, discarded")
        }
        Outcome::UnknownFrameType(t) => {
            eprintln!("on9log: unknown transport frame type 0x{t:02x}, discarded");
        }
        Outcome::InvalidEscape => eprintln!("on9log: invalid SLIP escape, discarded"),
        Outcome::PlainText(bytes) => {
            let mut out = std::io::stdout().lock();
            let _ = write_plain_text(&mut out, &bytes, timestamp, plain_text);
            if let Some(save) = save.as_deref_mut() {
                let _ = write_plain_text_saved(save, &bytes, timestamp, save_plain_text);
            }
            for annotation in crash_decoder.feed(&bytes, elf) {
                let _ = write_plain_annotation(&mut out, &annotation, timestamp);
                if let Some(save) = save.as_deref_mut() {
                    let _ =
                        save.write_line(&format!("{}{}", timestamp_prefix(timestamp), annotation));
                }
            }
            let _ = out.flush();
        }
    }
}

struct PlainTextState {
    line_start: bool,
    ansi: AnsiStripState,
}

impl PlainTextState {
    fn new() -> Self {
        Self {
            line_start: true,
            ansi: AnsiStripState::Ground,
        }
    }

    fn reset_line(&mut self) {
        self.line_start = true;
        self.ansi = AnsiStripState::Ground;
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum AnsiStripState {
    Ground,
    Escape,
    Csi,
}

fn warn_line(msg: &str, use_color: bool, timestamp: bool) {
    let msg = format!("{}{}", timestamp_prefix(timestamp), msg);
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

fn print_hexdump(bytes: &[u8], color_code: &str, use_color: bool, timestamp: bool) {
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
        let line = format!(
            "{}{:08x}  {:<48}  {}",
            timestamp_prefix(timestamp),
            addr,
            hex.join(" "),
            ascii
        );
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

fn write_hexdump_to_file(save: &mut SaveLog, bytes: &[u8], timestamp: bool) -> std::io::Result<()> {
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
        save.write_line(&format!(
            "{}{:08x}  {:<48}  {}",
            timestamp_prefix(timestamp),
            addr,
            hex.join(" "),
            ascii
        ))?;
    }
    Ok(())
}

fn write_plain_text<W: Write>(
    out: &mut W,
    bytes: &[u8],
    timestamp: bool,
    state: &mut PlainTextState,
) -> std::io::Result<()> {
    for &b in bytes {
        if timestamp && state.line_start {
            out.write_all(timestamp_prefix(true).as_bytes())?;
            state.line_start = false;
        }
        out.write_all(&[b])?;
        if b == b'\n' {
            state.line_start = true;
        }
    }
    Ok(())
}

fn write_plain_annotation<W: Write>(
    out: &mut W,
    line: &str,
    timestamp: bool,
) -> std::io::Result<()> {
    out.write_all(timestamp_prefix(timestamp).as_bytes())?;
    out.write_all(line.as_bytes())?;
    out.write_all(b"\n")
}

fn write_plain_text_saved(
    save: &mut SaveLog,
    bytes: &[u8],
    timestamp: bool,
    state: &mut PlainTextState,
) -> std::io::Result<()> {
    let clean = strip_ansi_stream(bytes, &mut state.ansi);
    let mut out = Vec::with_capacity(clean.len() + 32);
    for &b in &clean {
        if timestamp && state.line_start {
            out.extend_from_slice(timestamp_prefix(true).as_bytes());
            state.line_start = false;
        }
        out.push(b);
        if b == b'\n' {
            state.line_start = true;
        }
    }
    save.write_all_immediate(&out)?;
    Ok(())
}

#[cfg(test)]
fn strip_ansi(bytes: &[u8]) -> Vec<u8> {
    let mut state = AnsiStripState::Ground;
    strip_ansi_stream(bytes, &mut state)
}

fn strip_ansi_stream(bytes: &[u8], state: &mut AnsiStripState) -> Vec<u8> {
    let mut out = Vec::with_capacity(bytes.len());
    for &b in bytes {
        match *state {
            AnsiStripState::Ground => {
                if b == 0x1b {
                    *state = AnsiStripState::Escape;
                } else {
                    out.push(b);
                }
            }
            AnsiStripState::Escape => {
                if b == b'[' {
                    *state = AnsiStripState::Csi;
                } else {
                    *state = AnsiStripState::Ground;
                }
            }
            AnsiStripState::Csi => {
                if (0x40..=0x7e).contains(&b) {
                    *state = AnsiStripState::Ground;
                }
            }
        }
    }
    out
}

fn timestamp_prefix(enabled: bool) -> String {
    if !enabled {
        return String::new();
    }
    format!("[{}] ", local_timestamp())
}

#[cfg(unix)]
fn local_timestamp() -> String {
    let mut tv = libc::timeval {
        tv_sec: 0,
        tv_usec: 0,
    };
    unsafe {
        libc::gettimeofday(&mut tv, ptr::null_mut());
    }

    let secs: libc::time_t = tv.tv_sec;
    let mut tm = std::mem::MaybeUninit::<libc::tm>::uninit();
    let tm = unsafe {
        if libc::localtime_r(&secs, tm.as_mut_ptr()).is_null() {
            return "19700101-00:00:00.000".to_string();
        }
        tm.assume_init()
    };
    let millis = tv.tv_usec / 1000;

    format!(
        "{:04}{:02}{:02}-{:02}:{:02}:{:02}.{:03}",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        millis
    )
}

#[cfg(not(unix))]
fn local_timestamp() -> String {
    "19700101-00:00:00.000".to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timestamps_each_plain_text_line() {
        let mut out = Vec::new();
        let mut state = PlainTextState::new();
        write_plain_text(&mut out, b"a\nb", true, &mut state).unwrap();
        let s = String::from_utf8(out).unwrap();

        assert_eq!(s.matches('[').count(), 2);
        assert!(s.ends_with("b"));
        assert!(!state.line_start);
    }

    #[test]
    fn plain_text_timestamp_state_spans_chunks() {
        let mut out = Vec::new();
        let mut state = PlainTextState::new();
        write_plain_text(&mut out, b"a", true, &mut state).unwrap();
        write_plain_text(&mut out, b"b\n", true, &mut state).unwrap();
        let s = String::from_utf8(out).unwrap();

        assert_eq!(s.matches('[').count(), 1);
        assert!(state.line_start);
    }

    #[test]
    fn preserves_device_ansi_plain_text_colors() {
        let mut out = Vec::new();
        let mut state = PlainTextState::new();
        write_plain_text(
            &mut out,
            b"\x1b[0;32mI (1519) demo: esp_log plain text main heartbeat 2\x1b[0m\n",
            false,
            &mut state,
        )
        .unwrap();

        assert_eq!(
            out,
            b"\x1b[0;32mI (1519) demo: esp_log plain text main heartbeat 2\x1b[0m\n"
        );
    }

    #[test]
    fn does_not_infer_plain_text_color_from_prefix() {
        let mut out = Vec::new();
        let mut state = PlainTextState::new();
        write_plain_text(&mut out, b"I (1519) demo: uncolored\n", false, &mut state).unwrap();

        assert_eq!(out, b"I (1519) demo: uncolored\n");
    }

    #[test]
    fn plain_annotation_honors_timestamp_flag() {
        let mut out = Vec::new();
        write_plain_annotation(&mut out, "--- 0x42000000: app_main", false).unwrap();
        assert_eq!(
            String::from_utf8(out).unwrap(),
            "--- 0x42000000: app_main\n"
        );
    }

    #[test]
    fn default_save_path_uses_unix_timestamp_shape() {
        let path = default_save_path();
        let name = path.file_name().unwrap().to_string_lossy();
        assert!(name.starts_with("on9log-"));
        assert!(name.ends_with(".log"));
        assert!(
            name["on9log-".len()..name.len() - ".log".len()]
                .chars()
                .all(|c| c.is_ascii_digit())
        );
    }

    #[test]
    fn strip_ansi_removes_color_sequences() {
        assert_eq!(
            strip_ansi(b"\x1b[0;32mI (1) tag: colored\x1b[0m\n"),
            b"I (1) tag: colored\n"
        );
    }

    #[test]
    fn strip_ansi_stream_handles_split_sequences() {
        let mut state = AnsiStripState::Ground;
        let mut out = strip_ansi_stream(b"\x1b[0;", &mut state);
        assert_eq!(out, b"");
        out.extend(strip_ansi_stream(b"32mgreen\x1b", &mut state));
        out.extend(strip_ansi_stream(b"[0m\n", &mut state));
        assert_eq!(out, b"green\n");
        assert_eq!(state, AnsiStripState::Ground);
    }

    #[test]
    fn monitor_keys_quit_on_ctrl_rbracket_then_ctrl_t() {
        let mut keys = MonitorKeyState::new();
        assert!(!keys.feed(&[CTRL_RBRACKET]));
        assert!(keys.feed(&[CTRL_T]));
    }

    #[test]
    fn monitor_keys_quit_sequence_can_span_chunks() {
        let mut keys = MonitorKeyState::new();
        assert!(!keys.feed(b"abc"));
        assert!(!keys.feed(&[CTRL_RBRACKET]));
        assert!(keys.feed(&[CTRL_T]));
    }

    #[test]
    fn monitor_keys_reset_on_other_byte() {
        let mut keys = MonitorKeyState::new();
        assert!(!keys.feed(&[CTRL_RBRACKET, b'x', CTRL_T]));
        assert!(!keys.feed(&[CTRL_T]));
    }
}
