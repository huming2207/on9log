#include "on9log_test.h"

#include <cstdio>

namespace on9t {

static bool s_case_failed;
static bool s_case_skipped;
static int s_total_run;
static int s_total_failed;
static int s_total_skipped;

bool check(bool ok, const char *expr, const char *file, int line)
{
    if (!ok) {
        s_case_failed = true;
        std::printf("    CHECK FAILED %s:%d: %s\n", file, line, expr);
    }
    return ok;
}

void skip(const char *reason)
{
    s_case_skipped = true;
    std::printf("    skipped: %s\n", reason);
}

/* Leaves the logger as the next case expects it, even after a failed case. */
static void reset_logger()
{
    (void)capture_stop();
    on9log_set_level(ON9_LOG_LEVEL_VERBOSE);
    /* Any log attempt reports pending drops; with only platform sinks left, nobody checks it. */
    if (on9log_get_dropped_count() != 0) {
        on9log_write(ON9_LOG_LEVEL_ERROR, "on9t", "flush dropped count", "");
    }
}

int run(const char *suite, const test_case *cases, size_t count)
{
    int failed = 0;

    for (size_t i = 0; i < count; ++i) {
        s_case_failed = false;
        s_case_skipped = false;
        reset_logger();
        cases[i].run();
        reset_logger();

        const char *result = s_case_failed ? "FAIL" : (s_case_skipped ? "SKIP" : "PASS");
        std::printf("[%s] %s.%s\n", result, suite, cases[i].name);
        std::fflush(stdout);
        ++s_total_run;
        if (s_case_failed) {
            ++failed;
            ++s_total_failed;
        } else if (s_case_skipped) {
            ++s_total_skipped;
        }
    }
    return failed;
}

int print_summary()
{
    std::printf("ON9T RESULT: %s (%d run, %d failed, %d skipped)\n",
                s_total_failed == 0 ? "PASS" : "FAIL",
                s_total_run,
                s_total_failed,
                s_total_skipped);
    std::fflush(stdout);
    return s_total_failed;
}

static uint16_t le16(const std::vector<uint8_t> &bytes, size_t offset)
{
    if (!check(offset + 2 <= bytes.size(), "le16 read in range", __FILE__, __LINE__)) {
        return 0;
    }
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

/* Header: magic | type_level | seq u16 | time_ms u32 | tag_id u32 | fmt_id u32 | payload_len u16 */
uint16_t packet::seq() const { return le16(header, 2); }
uint32_t packet::time_ms() const { return le32(header, 4); }
uint32_t packet::tag_id() const { return le32(header, 8); }
uint32_t packet::fmt_id() const { return le32(header, 12); }
uint16_t packet::payload_len() const { return le16(header, 16); }

/*
 * Capture sink. Sinks run under the emit lock, so the callbacks never race
 * each other. The test reads s_packets only after capture_stop(), whose
 * on9log_remove_sink() waits for any packet in flight; s_ended can be polled
 * at any time.
 */
static std::vector<packet> s_packets;
static std::atomic<size_t> s_ended{0};

static void capture_start_cb(const uint8_t *header, size_t header_len, void *)
{
    packet p;
    if (header != nullptr) {
        p.header.assign(header, header + header_len);
    }
    s_packets.push_back(p);
}

static void capture_payload_cb(const uint8_t *payload, size_t len, size_t total_arg_cnt, size_t arg_index, void *)
{
    packet &p = s_packets.back();
    p.payload.insert(p.payload.end(), payload, payload + len);
    p.calls.push_back({len, total_arg_cnt, arg_index});
}

static void capture_end_cb(void *)
{
    s_packets.back().ended = true;
    s_ended.fetch_add(1, std::memory_order_release);
}

static const on9log_sink_t s_capture_sink = {
    .start_cb = capture_start_cb,
    .payload_cb = capture_payload_cb,
    .end_cb = capture_end_cb,
};

void capture_start()
{
    (void)on9log_remove_sink(&s_capture_sink, nullptr);
    s_packets.clear();
    s_ended.store(0, std::memory_order_relaxed);
    (void)check(on9log_add_sink(&s_capture_sink, nullptr) == ON9LOG_OK, "capture sink added", __FILE__, __LINE__);
}

std::vector<packet> capture_stop()
{
    (void)on9log_remove_sink(&s_capture_sink, nullptr);
    std::vector<packet> packets;
    packets.swap(s_packets);
    return packets;
}

size_t captured_count()
{
    return s_ended.load(std::memory_order_acquire);
}

uint32_t le32(const std::vector<uint8_t> &bytes, size_t offset)
{
    if (!check(offset + 4 <= bytes.size(), "le32 read in range", __FILE__, __LINE__)) {
        return 0;
    }
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

uint64_t le64(const std::vector<uint8_t> &bytes, size_t offset)
{
    return static_cast<uint64_t>(le32(bytes, offset)) | (static_cast<uint64_t>(le32(bytes, offset + 4)) << 32);
}

void put32(std::vector<uint8_t> &bytes, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

void put64(std::vector<uint8_t> &bytes, uint64_t value)
{
    put32(bytes, static_cast<uint32_t>(value));
    put32(bytes, static_cast<uint32_t>(value >> 32));
}

void put_string(std::vector<uint8_t> &bytes, const std::string &value)
{
    put32(bytes, static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void dump(const char *label, const std::vector<uint8_t> &bytes)
{
    std::printf("    %s (%u bytes):", label, static_cast<unsigned>(bytes.size()));
    for (uint8_t byte : bytes) {
        std::printf(" %02x", byte);
    }
    std::printf("\n");
}

/* CRC-16-CCITT, initial value 0xffff, written out from the spec rather than shared with the code under test. */
static uint16_t crc16_ccitt(const std::vector<uint8_t> &bytes)
{
    uint16_t crc = 0xffff;
    for (uint8_t byte : bytes) {
        crc ^= static_cast<uint16_t>(byte << 8);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) != 0 ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

std::vector<frame> decode_frames(const std::vector<uint8_t> &stream)
{
    std::vector<frame> frames;
    size_t i = 0;
    while (i < stream.size()) {
        if (stream[i++] != 0xa5) {
            continue; // text outside frames
        }
        // Unescape up to the end byte: type, payload, then the CRC (little-endian).
        std::vector<uint8_t> body;
        bool clean = true;
        bool ended = false;
        while (i < stream.size() && !ended) {
            uint8_t byte = stream[i++];
            if (byte == 0xc0) {
                ended = true;
            } else if (byte == 0xdb && i < stream.size()) {
                switch (stream[i++]) {
                case 0xde: body.push_back(0xa5); break;
                case 0xdc: body.push_back(0xc0); break;
                case 0xdd: body.push_back(0xdb); break;
                case 0xd0: body.push_back(0x0d); break;
                case 0xd1: body.push_back(0x0a); break;
                default: clean = false; break;
                }
            } else {
                clean = clean && byte != 0xa5 && byte != 0x0a && byte != 0x0d && byte != 0xdb;
                body.push_back(byte);
            }
        }

        frame f;
        if (ended && body.size() >= 3) {
            const uint16_t crc = static_cast<uint16_t>(body[body.size() - 2] | (body[body.size() - 1] << 8));
            body.resize(body.size() - 2);
            f.type = body[0];
            f.payload.assign(body.begin() + 1, body.end());
            f.valid = clean && crc == crc16_ccitt(body);
        }
        frames.push_back(f);
    }
    return frames;
}

} // namespace on9t
