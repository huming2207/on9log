#pragma once

/*
 * A small test harness shared by the Linux unit test and the ESP32 QEMU test
 * app. It has no dependencies, so the same cases run on both platforms.
 *
 * A case is a plain function. ON9T_CHECK stops the case at the first failed
 * check; the runner then cleans up (removes the capture sink, restores the
 * level, flushes the dropped count) so one failure does not spill into the
 * next case.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "on9log.h"
#include "on9log_fmt.h"

namespace on9t {

struct test_case {
    const char *name;
    void (*run)();
};

/** Records a failed check and prints where it was. Returns @p ok. */
bool check(bool ok, const char *expr, const char *file, int line);

/** Marks the current case as skipped, with the reason printed. */
void skip(const char *reason);

/**
 * Runs @p count cases, printing one line per case, and returns how many
 * failed. Totals across calls are kept for print_summary().
 */
int run(const char *suite, const test_case *cases, size_t count);

/** Prints the final "ON9T RESULT" line and returns the total failure count. */
int print_summary();

#define ON9T_CHECK(cond)                                           \
    do {                                                           \
        if (!on9t::check((cond), #cond, __FILE__, __LINE__)) {     \
            return;                                                \
        }                                                          \
    } while (0)

#define ON9T_SKIP(reason)       \
    do {                        \
        on9t::skip(reason);     \
        return;                 \
    } while (0)

/** One payload_cb call, as the sink saw it. */
struct sink_call {
    size_t len;
    size_t total_arg_cnt;
    size_t arg_index;
};

/** One packet (binary mode) or one text line (plain-text mode) seen by the capture sink. */
struct packet {
    std::vector<uint8_t> header; /**< Empty in plain-text mode. */
    std::vector<uint8_t> payload;
    std::vector<sink_call> calls;
    bool ended = false;

    uint8_t type() const { return header[1] >> 4; }
    uint8_t level() const { return header[1] & 0x0f; }
    uint16_t seq() const;
    uint32_t time_ms() const;
    uint32_t tag_id() const;
    uint32_t fmt_id() const;
    uint16_t payload_len() const;
    std::string text() const { return std::string(payload.begin(), payload.end()); }
};

/** Registers the capture sink with an empty packet list. */
void capture_start();

/** Removes the capture sink and returns everything it saw. */
std::vector<packet> capture_stop();

/** Packets the capture sink has finished so far; safe to poll from another task. */
size_t captured_count();

/** Little-endian readers. Reading past the end fails the current case and returns 0. */
uint32_t le32(const std::vector<uint8_t> &bytes, size_t offset);
uint64_t le64(const std::vector<uint8_t> &bytes, size_t offset);

/** Bytes of a little-endian u32 / u64, for building expected payloads. */
void put32(std::vector<uint8_t> &bytes, uint32_t value);
void put64(std::vector<uint8_t> &bytes, uint64_t value);

/** Appends an inline/dynamic string: u32 length then the bytes. */
void put_string(std::vector<uint8_t> &bytes, const std::string &value);

/** Prints a byte vector as hex, for failure messages. */
void dump(const char *label, const std::vector<uint8_t> &bytes);

/** One typed SLIP transport frame (README "Default ESP VFS Sink"). */
struct frame {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    bool valid = false; /**< Escapes, CRC, and no raw CR/LF/start byte inside. */
};

/** Decodes every frame in a transport byte stream; bytes between frames are skipped. */
std::vector<frame> decode_frames(const std::vector<uint8_t> &stream);

/** Log two NTTP formats each; the first is the same in both (tests/on9log_test_tu_*.cpp). */
void tu_a_log();
void tu_b_log();

/** Cases that run on every platform (tests/on9log_test_cases.cpp). */
extern const test_case core_cases[];
extern const size_t core_case_count;

} // namespace on9t

/* C-macro log calls, compiled as C so the _Generic classifier is used (tests/on9log_test_c_calls.c). */
extern "C" {
void on9t_c_scalars(void);
void on9t_c_strings(const char *text, const char *null_text);
void on9t_c_precision(const char *text);
void on9t_c_double(double d, float f);
void on9t_c_long(long value, unsigned long uvalue);
void on9t_c_sixteen_args(void);
void on9t_c_all_levels(void);
void on9t_c_buffer(const void *data, size_t len);
void on9t_c_isr(uint32_t value);
}
