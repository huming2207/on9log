/*
 * The Unix stdio sink, end to end: logs go to a temporary file, which is then
 * read back and decoded. A separate program from on9log_unit because the sink,
 * once installed, stays for the life of the process.
 */

#include "on9log_test.h"

#include <cstdio>
#include <cstring>

#include "on9log.hpp"
#include "on9log_transport.h"
#include "on9log_unix_stdio.h"

namespace {

FILE *g_output;
long g_read_from;

/** Everything written to the sink since the last call. */
std::vector<uint8_t> new_output()
{
    std::vector<uint8_t> bytes;
    if (std::fflush(g_output) != 0 || std::fseek(g_output, g_read_from, SEEK_SET) != 0) {
        return bytes;
    }
    int c;
    while ((c = std::fgetc(g_output)) != EOF) {
        bytes.push_back(static_cast<uint8_t>(c));
    }
    g_read_from = std::ftell(g_output);
    return bytes;
}

#if !ON9LOG_PLAIN_TEXT
void binary_packets_are_framed()
{
    // These bytes are the frame's own markers and the console's newline bytes.
    const uint8_t special[] = {0xa5, 0xc0, 0xdb, 0x0a, 0x0d, 0x9a};
    Logger log("unix-smoke");
    log.info("value=%d", 42);
    log.buffer_info(special, sizeof(special));

    const auto frames = on9t::decode_frames(new_output());
    ON9T_CHECK(frames.size() == 2);
    for (const auto &frame : frames) {
        ON9T_CHECK(frame.valid);
        ON9T_CHECK(frame.type == 0x01);
        ON9T_CHECK(frame.payload.size() >= sizeof(on9log_packet_header_t));
        ON9T_CHECK(frame.payload[0] == ON9LOG_PACKET_MAGIC);
        const size_t payload_len = frame.payload[16] | (frame.payload[17] << 8);
        ON9T_CHECK(frame.payload.size() == sizeof(on9log_packet_header_t) + payload_len);
    }
    // The buffer bytes survive escaping unchanged.
    const auto &buffer = frames[1].payload;
    ON9T_CHECK(std::memcmp(&buffer[buffer.size() - sizeof(special)], special, sizeof(special)) == 0);
}

void oversized_packets_are_dropped_whole()
{
    std::vector<uint8_t> oversized(ON9LOG_TRANSPORT_MAX_PAYLOAD + 1u, 0);
    oversized[0] = ON9LOG_PACKET_MAGIC;
    ON9T_CHECK(on9log_dispatch_packet(oversized.data(), oversized.size()) == ON9LOG_OK);
    ON9T_CHECK(new_output().empty());

    std::vector<uint8_t> largest(ON9LOG_TRANSPORT_MAX_PAYLOAD, 0);
    largest[0] = ON9LOG_PACKET_MAGIC;
    ON9T_CHECK(on9log_dispatch_packet(largest.data(), largest.size()) == ON9LOG_OK);
    const auto frames = on9t::decode_frames(new_output());
    ON9T_CHECK(frames.size() == 1 && frames[0].valid && frames[0].payload.size() == largest.size());
}
#else
void text_lines_are_written_as_is()
{
    Logger log("unix-smoke");
    log.info("value=%d", 42);
    log.info("name={} value={}", "host", 42);
    const auto bytes = new_output();
    const std::string text(bytes.begin(), bytes.end());
    ON9T_CHECK(text.find("[unix-smoke]") != std::string::npos);
    ON9T_CHECK(text.find("value=42") != std::string::npos);
    ON9T_CHECK(text.find("name=host value=42") != std::string::npos);
    ON9T_CHECK(!text.empty() && text.back() == '\n');
}
#endif

const on9t::test_case smoke_cases[] = {
#if !ON9LOG_PLAIN_TEXT
    {"binary_packets_are_framed", binary_packets_are_framed},
    {"oversized_packets_are_dropped_whole", oversized_packets_are_dropped_whole},
#else
    {"text_lines_are_written_as_is", text_lines_are_written_as_is},
#endif
};

} // namespace

int main()
{
    g_output = std::tmpfile();
    if (g_output == nullptr || on9log_unix_stdio_init_file(g_output) != ON9LOG_OK ||
        on9log_unix_stdio_init_file(g_output) != ON9LOG_OK) { // the second call is a no-op
        return 1;
    }
    on9t::run("unix_stdio", smoke_cases, sizeof(smoke_cases) / sizeof(smoke_cases[0]));
    return on9t::print_summary() == 0 ? 0 : 1;
}
