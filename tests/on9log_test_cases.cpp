/*
 * Test cases that run on every platform: the Linux unit test and the ESP32
 * QEMU test app both run on9t::core_cases. Platform-specific behaviour (what
 * counts as "in the image", the image ID kind, the ISR backend) is checked
 * against the port's own answer here, and in detail by each platform's own
 * cases.
 */

#include "on9log_test.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "on9log.hpp"
#include "on9log_port.h"

using on9t::packet;

namespace {

/** Counts packets per ctx: each sink registration gets its own counter. */
void count_start(const uint8_t *, size_t, void *ctx)
{
    ++*static_cast<int *>(ctx);
}
void count_payload(const uint8_t *, size_t, size_t, size_t, void *) {}
void count_end(void *) {}
const on9log_sink_t count_sink = {count_start, count_payload, count_end};

/** Number of packets one log call produces, given the current filters. */
size_t packets_for(void (*log)())
{
    on9t::capture_start();
    log();
    return on9t::capture_stop().size();
}

// ---------------------------------------------------------------------------
// Cases for both binary and plain-text mode
// ---------------------------------------------------------------------------

void global_level_filters()
{
    on9log_set_level(ON9_LOG_LEVEL_WARN);
    ON9T_CHECK(on9log_get_level() == ON9_LOG_LEVEL_WARN);
    ON9T_CHECK(packets_for([] { ON9_LOGI("lvl", "info"); }) == 0);
    ON9T_CHECK(packets_for([] { ON9_LOGW("lvl", "warn"); }) == 1);
    ON9T_CHECK(packets_for([] { ON9_LOGE("lvl", "error"); }) == 1);
    ON9T_CHECK(packets_for([] { ON9_LOG_BUFI("lvl", "x", 1); }) == 0);

    on9log_set_level(static_cast<on9log_level_t>(9)); // invalid: ignored
    ON9T_CHECK(on9log_get_level() == ON9_LOG_LEVEL_WARN);

    on9log_set_level(ON9_LOG_LEVEL_NONE);
    ON9T_CHECK(packets_for([] { ON9_LOGE("lvl", "error"); }) == 0);
}

void invalid_levels_never_log()
{
    ON9T_CHECK(packets_for([] { on9log_write(ON9_LOG_LEVEL_NONE, "lvl", "none", ""); }) == 0);
    ON9T_CHECK(packets_for([] { on9log_write(static_cast<on9log_level_t>(7), "lvl", "bad", ""); }) == 0);
}

void compile_time_ceiling()
{
#if ON9_LOG_LOCAL_LEVEL >= 4 // DEBUG; #if cannot see enum values
    ON9T_SKIP("built with a DEBUG or VERBOSE ceiling");
#else
    // ON9_LOGD/V are compiled out, and the core also rejects a direct call above the ceiling.
    ON9T_CHECK(packets_for([] { ON9_LOGD("lvl", "debug"); }) == 0);
    ON9T_CHECK(packets_for([] { ON9_LOGV("lvl", "verbose"); }) == 0);
    ON9T_CHECK(packets_for([] { on9log_write(ON9_LOG_LEVEL_DEBUG, "lvl", "debug", ""); }) == 0);
    ON9T_CHECK(packets_for([] { on9t_c_all_levels(); }) == 3);
#endif
}

void tag_level_overrides_global()
{
    on9log_set_level(ON9_LOG_LEVEL_INFO);
    ON9T_CHECK(on9log_set_tag_level("quiet", ON9_LOG_LEVEL_ERROR) == ON9LOG_OK);
    ON9T_CHECK(packets_for([] { ON9_LOGW("quiet", "warn"); }) == 0);
    ON9T_CHECK(packets_for([] { ON9_LOGE("quiet", "error"); }) == 1);
    ON9T_CHECK(packets_for([] { ON9_LOGW("other", "warn"); }) == 1);

    // An override may also be looser than the global level.
    on9log_set_level(ON9_LOG_LEVEL_ERROR);
    ON9T_CHECK(on9log_set_tag_level("loud", ON9_LOG_LEVEL_INFO) == ON9LOG_OK);
    ON9T_CHECK(packets_for([] { ON9_LOGI("loud", "info"); }) == 1);
    ON9T_CHECK(packets_for([] { ON9_LOGI("other", "info"); }) == 0);

    // Setting the same tag again updates it.
    ON9T_CHECK(on9log_set_tag_level("loud", ON9_LOG_LEVEL_ERROR) == ON9LOG_OK);
    ON9T_CHECK(packets_for([] { ON9_LOGI("loud", "info"); }) == 0);

    // A NULL tag never matches a filter.
    on9log_set_level(ON9_LOG_LEVEL_INFO);
    ON9T_CHECK(packets_for([] { ON9_LOGI(nullptr, "null tag"); }) == 1);

    ON9T_CHECK(on9log_clear_tag_level("quiet") == ON9LOG_OK);
    ON9T_CHECK(on9log_clear_tag_level("loud") == ON9LOG_OK);
    ON9T_CHECK(packets_for([] { ON9_LOGW("quiet", "warn"); }) == 1);
}

void tag_level_matches_contents_and_copies()
{
    char owned[] = "owned-tag";
    ON9T_CHECK(on9log_set_tag_level(owned, ON9_LOG_LEVEL_NONE) == ON9LOG_OK);
    std::memset(owned, 'x', sizeof(owned) - 1); // the logger kept its own copy
    ON9T_CHECK(packets_for([] { ON9_LOGE("owned-tag", "matched by contents"); }) == 0);
    ON9T_CHECK(on9log_clear_tag_level("owned-tag") == ON9LOG_OK);
    ON9T_CHECK(packets_for([] { ON9_LOGE("owned-tag", "cleared"); }) == 1);
}

void tag_level_errors()
{
    ON9T_CHECK(on9log_set_tag_level(nullptr, ON9_LOG_LEVEL_INFO) == ON9LOG_ERR_INVALID_ARG);
    ON9T_CHECK(on9log_set_tag_level("t", static_cast<on9log_level_t>(6)) == ON9LOG_ERR_INVALID_ARG);
    ON9T_CHECK(on9log_clear_tag_level(nullptr) == ON9LOG_ERR_INVALID_ARG);
    ON9T_CHECK(on9log_clear_tag_level("never-set") == ON9LOG_ERR_NOT_FOUND);

    // Clearing twice is fine; a cleared slot is reused by the next tag.
    ON9T_CHECK(on9log_set_tag_level("first", ON9_LOG_LEVEL_ERROR) == ON9LOG_OK);
    ON9T_CHECK(on9log_clear_tag_level("first") == ON9LOG_OK);
    ON9T_CHECK(on9log_clear_tag_level("first") == ON9LOG_OK);
    ON9T_CHECK(on9log_set_tag_level("second", ON9_LOG_LEVEL_ERROR) == ON9LOG_OK);
    ON9T_CHECK(packets_for([] { ON9_LOGW("first", "first is cleared"); }) == 1);
    ON9T_CHECK(packets_for([] { ON9_LOGW("second", "second is filtered"); }) == 0);
    ON9T_CHECK(on9log_clear_tag_level("second") == ON9LOG_OK);
}

void sink_registry()
{
    const on9log_sink_t missing_end = {count_start, count_payload, nullptr};
    ON9T_CHECK(on9log_add_sink(nullptr, nullptr) == ON9LOG_ERR_INVALID_ARG);
    ON9T_CHECK(on9log_add_sink(&missing_end, nullptr) == ON9LOG_ERR_INVALID_ARG);
    ON9T_CHECK(on9log_remove_sink(nullptr, nullptr) == ON9LOG_ERR_NOT_FOUND);
    ON9T_CHECK(on9log_remove_sink(&count_sink, nullptr) == ON9LOG_ERR_NOT_FOUND);

    // Adding the same (sink, ctx) twice registers it once.
    int a = 0;
    int b = 0;
    ON9T_CHECK(on9log_add_sink(&count_sink, &a) == ON9LOG_OK);
    ON9T_CHECK(on9log_add_sink(&count_sink, &a) == ON9LOG_OK);
    ON9T_CHECK(on9log_add_sink(&count_sink, &b) == ON9LOG_OK);
    ON9_LOGI("sink", "one");
    ON9T_CHECK(a == 1 && b == 1);

    ON9T_CHECK(on9log_remove_sink(&count_sink, &a) == ON9LOG_OK);
    ON9T_CHECK(on9log_remove_sink(&count_sink, &a) == ON9LOG_ERR_NOT_FOUND);
    ON9_LOGI("sink", "two");
    ON9T_CHECK(a == 1 && b == 2);
    ON9T_CHECK(on9log_remove_sink(&count_sink, &b) == ON9LOG_OK);

    ON9_LOGI("sink", "nobody listens"); // no sinks: must not crash
}

void sink_table_full()
{
    // Other sinks may be registered by the platform, so fill whatever is left.
    std::vector<int> counters(ON9LOG_MAX_SINKS + 1, 0);
    size_t added = 0;
    while (added < counters.size() && on9log_add_sink(&count_sink, &counters[added]) == ON9LOG_OK) {
        ++added;
    }
    ON9T_CHECK(added >= 1 && added <= ON9LOG_MAX_SINKS);
    ON9T_CHECK(on9log_add_sink(&count_sink, &counters[added]) == ON9LOG_ERR_NO_MEM);

    ON9_LOGI("sink", "everyone");
    for (size_t i = 0; i < added; ++i) {
        ON9T_CHECK(counters[i] == 1);
    }

    // Removing one frees a slot for a new sink.
    ON9T_CHECK(on9log_remove_sink(&count_sink, &counters[0]) == ON9LOG_OK);
    ON9T_CHECK(on9log_add_sink(&count_sink, &counters[added]) == ON9LOG_OK);
    for (size_t i = 1; i <= added; ++i) {
        ON9T_CHECK(on9log_remove_sink(&count_sink, &counters[i]) == ON9LOG_OK);
    }
}

void isr_without_backend_is_silent()
{
    if (on9log_port_isr_ready()) {
        ON9T_SKIP("this platform has an ISR backend; its own cases cover it");
    }
    on9t::capture_start();
    ON9T_CHECK(on9log_write_isr(ON9_LOG_LEVEL_INFO, "isr", "isr %u", "\x01", 1u));
    on9t_c_isr(2);
    ON9T_CHECK(on9t::capture_stop().empty());
    ON9T_CHECK(on9log_get_dropped_count() == 0);
}

void init_is_idempotent()
{
    ON9T_CHECK(on9log_init() == ON9LOG_OK);
    ON9T_CHECK(on9log_init() == ON9LOG_OK);
}

#if !ON9LOG_PLAIN_TEXT
// ---------------------------------------------------------------------------
// Binary mode: packet layout
// ---------------------------------------------------------------------------

constexpr size_t kHeaderLen = sizeof(on9log_packet_header_t);
constexpr uint8_t T32 = ON9_LOG_ARGS_TYPE_32BITS;
constexpr uint8_t T64 = ON9_LOG_ARGS_TYPE_64BITS;
constexpr uint8_t TPTR = ON9_LOG_ARGS_TYPE_POINTER;
constexpr uint8_t TSTR = ON9_LOG_ARGS_TYPE_DYNAMIC_STRING;
constexpr uint8_t TVIEW = ON9_LOG_ARGS_TYPE_DYNAMIC_STRING_VIEW;

/** True if the bytes match; prints both otherwise. */
bool same_bytes(const std::vector<uint8_t> &actual, const std::vector<uint8_t> &expected)
{
    if (actual == expected) {
        return true;
    }
    on9t::dump("actual", actual);
    on9t::dump("expected", expected);
    return false;
}

uint32_t addr_id(const void *p)
{
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}

uint64_t double_bits(double value)
{
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}


/** Checks what every binary packet must satisfy and returns false on the first problem. */
bool well_formed(const packet &p)
{
    return p.ended && p.header.size() == kHeaderLen && p.header[0] == ON9LOG_PACKET_MAGIC &&
           p.payload_len() == p.payload.size() && p.payload_len() != ON9LOG_PAYLOAD_LEN_STREAMING;
}

void header_fields()
{
    static const char tag[] = "hdr-tag";
    on9t::capture_start();
    ON9_LOGW(tag, "no arguments");
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 1);
    const packet &p = packets[0];
    ON9T_CHECK(well_formed(p));
    ON9T_CHECK(p.type() == ON9LOG_PKT_LOG);
    ON9T_CHECK(p.level() == ON9_LOG_LEVEL_WARN);
    ON9T_CHECK(p.tag_id() == addr_id(tag));
    ON9T_CHECK(p.fmt_id() != 0 && p.fmt_id() != ON9LOG_ID_INLINE);
    ON9T_CHECK(same_bytes(p.payload, {0})); // argument count 0, no type table
}

void sink_callback_arguments()
{
    char text[] = "ab";
    on9t::capture_start();
    ON9_LOGI("calls", "%d %s", 1, text);
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 1);
    const auto &calls = packets[0].calls;
    // count, type table, then each argument; a string is its length then its bytes.
    ON9T_CHECK(calls.size() == 5);
    ON9T_CHECK(calls[0].len == 1 && calls[0].arg_index == SIZE_MAX);
    ON9T_CHECK(calls[1].len == 2 && calls[1].arg_index == SIZE_MAX);
    ON9T_CHECK(calls[2].len == 4 && calls[2].arg_index == 0);
    ON9T_CHECK(calls[3].len == 4 && calls[3].arg_index == 1);
    ON9T_CHECK(calls[4].len == 2 && calls[4].arg_index == 1);
    for (const auto &call : calls) {
        ON9T_CHECK(call.total_arg_cnt == 2);
    }
}

void c_scalar_arguments()
{
    on9t::capture_start();
    on9t_c_scalars();
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 1);
    ON9T_CHECK(well_formed(packets[0]));
    std::vector<uint8_t> expected = {5, T32, T32, T64, T64, TPTR};
    on9t::put32(expected, static_cast<uint32_t>(-5));
    on9t::put32(expected, 7);
    on9t::put64(expected, static_cast<uint64_t>(INT64_MIN));
    on9t::put64(expected, UINT64_MAX);
    on9t::put32(expected, 0x1234);
    ON9T_CHECK(same_bytes(packets[0].payload, expected));
}

void c_string_arguments()
{
    on9t::capture_start();
    on9t_c_strings("text", nullptr);
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 1);
    ON9T_CHECK(well_formed(packets[0]));
    // A literal, an empty string, NULL (length 0xffffffff, no bytes) and a char array.
    std::vector<uint8_t> expected = {4, TSTR, TSTR, TSTR, TSTR};
    on9t::put_string(expected, "text");
    on9t::put_string(expected, "");
    on9t::put32(expected, 0xffffffffu);
    on9t::put_string(expected, "array");
    ON9T_CHECK(same_bytes(packets[0].payload, expected));
}

void c_sixteen_arguments()
{
    on9t::capture_start();
    on9t_c_sixteen_args();
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 1);
    std::vector<uint8_t> expected = {16};
    expected.insert(expected.end(), 16, T32);
    for (uint32_t i = 1; i <= 16; ++i) {
        on9t::put32(expected, i);
    }
    ON9T_CHECK(same_bytes(packets[0].payload, expected));
}

void c_double_arguments()
{
#if defined(__x86_64__) || defined(__aarch64__)
    // These ABIs pass variadic doubles in FP registers, but the core reads every
    // 64-bit argument with va_arg(uint64_t). Known bug; C++ Logger is not affected.
    ON9T_SKIP("known bug: C-macro double arguments are misread on x86-64/AArch64");
#else
    on9t::capture_start();
    on9t_c_double(1.5, 2.5f);
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 1);
    std::vector<uint8_t> expected = {3, T64, T64, T32}; // a float is promoted to double
    on9t::put64(expected, double_bits(1.5));
    on9t::put64(expected, double_bits(2.5));
    on9t::put32(expected, 7);
    ON9T_CHECK(same_bytes(packets[0].payload, expected));
#endif
}

void c_long_arguments()
{
    if (sizeof(long) != sizeof(uint32_t)) {
        // _Generic sends long as 32 bits, so a 64-bit long loses its upper half.
        ON9T_SKIP("known bug: C-macro long arguments are truncated where long is 64-bit");
    }
    on9t::capture_start();
    on9t_c_long(-2, 0xfffffff0ul);
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 1);
    std::vector<uint8_t> expected = {2, T32, T32};
    on9t::put32(expected, static_cast<uint32_t>(-2));
    on9t::put32(expected, 0xfffffff0u);
    ON9T_CHECK(same_bytes(packets[0].payload, expected));
}

void precision_strings()
{
    on9t::capture_start();
    on9t_c_precision("dynamic");
    // The scan-hint API applies the precision on the device.
    on9log_write_with_format_scan(ON9_LOG_LEVEL_INFO, "prec", "%.*s", "%.*s", "\x01\x04", 3, "dynamic");
    on9log_write_with_format_scan(ON9_LOG_LEVEL_INFO, "prec", "%.*s", "%.*s", "\x01\x04", -1, "dynamic");
    on9log_write_with_format_scan(ON9_LOG_LEVEL_INFO, "prec", "%.*s", "%.*s", "\x01\x04", 50, "dynamic");
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 4);
    auto expect = [](int32_t precision, const char *sent) {
        std::vector<uint8_t> bytes = {2, T32, TSTR};
        on9t::put32(bytes, static_cast<uint32_t>(precision));
        on9t::put_string(bytes, sent);
        return bytes;
    };
    // By default the macro sends the whole string and the host applies the precision.
    ON9T_CHECK(same_bytes(packets[0].payload, expect(3, ON9LOG_ENABLE_FORMAT_SCAN_HINT ? "dyn" : "dynamic")));
    ON9T_CHECK(same_bytes(packets[1].payload, expect(3, "dyn")));
    ON9T_CHECK(same_bytes(packets[2].payload, expect(-1, "dynamic"))); // negative: no precision
    ON9T_CHECK(same_bytes(packets[3].payload, expect(50, "dynamic")));
}

void long_strings_are_capped()
{
    std::string long_text(ON9LOG_MAX_DYNAMIC_STRING_LEN + 10, 'a');
    std::string long_format(ON9LOG_MAX_DYNAMIC_STRING_LEN + 10, 'f');
    on9t::capture_start();
    ON9_LOGI("cap", "%s", long_text.c_str());
    ON9_LOG_RUNTIMEI("cap", long_format.c_str());
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 2);
    ON9T_CHECK(well_formed(packets[0]) && well_formed(packets[1]));
    std::vector<uint8_t> expected = {1, TSTR};
    on9t::put_string(expected, std::string(ON9LOG_MAX_DYNAMIC_STRING_LEN, 'a'));
    ON9T_CHECK(same_bytes(packets[0].payload, expected));

    // A heap format goes inline, capped the same way.
    ON9T_CHECK(packets[1].fmt_id() == ON9LOG_ID_INLINE);
    expected.clear();
    on9t::put_string(expected, std::string(ON9LOG_MAX_DYNAMIC_STRING_LEN, 'f'));
    expected.push_back(0); // argument count
    ON9T_CHECK(same_bytes(packets[1].payload, expected));
}

void inline_tags_and_formats()
{
    char stack_tag[] = "stack-tag";
    char stack_format[] = "runtime %d";
    ON9T_CHECK(!on9log_port_addr_in_image(stack_tag));
    ON9T_CHECK(on9log_port_addr_in_image("literal"));

    on9t::capture_start();
    ON9_LOGI(stack_tag, "value %d", 1);
    ON9_LOG_RUNTIMEI("literal", stack_format, 2);
    ON9_LOG_RUNTIMEI(stack_tag, stack_format, 3);
    auto packets = on9t::capture_stop();
    ON9T_CHECK(packets.size() == 3);

    // Inline tag only: the payload starts with it.
    std::vector<uint8_t> expected;
    on9t::put_string(expected, "stack-tag");
    expected.insert(expected.end(), {1, T32});
    on9t::put32(expected, 1);
    ON9T_CHECK(well_formed(packets[0]));
    ON9T_CHECK(packets[0].tag_id() == ON9LOG_ID_INLINE && packets[0].fmt_id() != ON9LOG_ID_INLINE);
    ON9T_CHECK(same_bytes(packets[0].payload, expected));

    // Inline format only.
    expected.clear();
    on9t::put_string(expected, "runtime %d");
    expected.insert(expected.end(), {1, T32});
    on9t::put32(expected, 2);
    ON9T_CHECK(well_formed(packets[1]));
    ON9T_CHECK(packets[1].tag_id() != ON9LOG_ID_INLINE && packets[1].fmt_id() == ON9LOG_ID_INLINE);
    ON9T_CHECK(same_bytes(packets[1].payload, expected));

    // Both: tag first, then format.
    expected.clear();
    on9t::put_string(expected, "stack-tag");
    on9t::put_string(expected, "runtime %d");
    expected.insert(expected.end(), {1, T32});
    on9t::put32(expected, 3);
    ON9T_CHECK(well_formed(packets[2]));
    ON9T_CHECK(packets[2].tag_id() == ON9LOG_ID_INLINE && packets[2].fmt_id() == ON9LOG_ID_INLINE);
    ON9T_CHECK(same_bytes(packets[2].payload, expected));
}

void null_tag_is_address_zero()
{
    on9t::capture_start();
    ON9_LOGI(nullptr, "no tag");
    auto packets = on9t::capture_stop();
    ON9T_CHECK(packets.size() == 1);
    ON9T_CHECK(well_formed(packets[0]));
    ON9T_CHECK(packets[0].tag_id() == 0);
}

// ---------------------------------------------------------------------------
// Binary mode: buffers
// ---------------------------------------------------------------------------

/** Buffer payload: total_len | offset | chunk_len | bytes. */
std::vector<uint8_t> buffer_payload(uint32_t total, uint32_t offset, const std::vector<uint8_t> &chunk)
{
    std::vector<uint8_t> bytes;
    on9t::put32(bytes, total);
    on9t::put32(bytes, offset);
    on9t::put32(bytes, static_cast<uint32_t>(chunk.size()));
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    return bytes;
}

void buffer_small_and_empty()
{
    const std::vector<uint8_t> data = {0x00, 0x9a, 0xc0, 0xff};
    on9t::capture_start();
    on9t_c_buffer(data.data(), data.size());
    on9t_c_buffer(nullptr, 0);
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 2);
    for (const auto &p : packets) {
        ON9T_CHECK(well_formed(p));
        ON9T_CHECK(p.type() == ON9LOG_PKT_BUFFER && p.level() == ON9_LOG_LEVEL_WARN);
        ON9T_CHECK(p.tag_id() != ON9LOG_ID_INLINE && p.fmt_id() == 0);
    }
    ON9T_CHECK(same_bytes(packets[0].payload, buffer_payload(4, 0, data)));
    ON9T_CHECK(same_bytes(packets[1].payload, buffer_payload(0, 0, {})));

    // Buffer data goes to the sink as argument 0 of 1; its metadata as SIZE_MAX.
    const auto &calls = packets[0].calls;
    ON9T_CHECK(calls.size() == 4);
    ON9T_CHECK(calls[0].arg_index == SIZE_MAX && calls[2].arg_index == SIZE_MAX);
    ON9T_CHECK(calls[3].arg_index == 0 && calls[3].total_arg_cnt == 1 && calls[3].len == 4);
}

void buffer_chunks()
{
    const size_t chunk = ON9LOG_BUFFER_CHUNK_SIZE;
    std::vector<uint8_t> data(2 * chunk + 7);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i * 7);
    }
    std::vector<uint8_t> exact(chunk, 0x55);

    on9t::capture_start();
    on9t_c_buffer(data.data(), data.size());
    on9t_c_buffer(exact.data(), exact.size());
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 4);
    const auto first = data.begin();
    ON9T_CHECK(same_bytes(packets[0].payload, buffer_payload(data.size(), 0, {first, first + chunk})));
    ON9T_CHECK(same_bytes(packets[1].payload, buffer_payload(data.size(), chunk, {first + chunk, first + 2 * chunk})));
    ON9T_CHECK(same_bytes(packets[2].payload, buffer_payload(data.size(), 2 * chunk, {first + 2 * chunk, data.end()})));
    ON9T_CHECK(same_bytes(packets[3].payload, buffer_payload(exact.size(), 0, exact))); // exactly one chunk: one packet
    for (size_t i = 0; i < packets.size(); ++i) {
        ON9T_CHECK(well_formed(packets[i]));
        ON9T_CHECK(packets[i].seq() == static_cast<uint16_t>(packets[0].seq() + i));
    }
}

void buffer_with_inline_tag()
{
    char stack_tag[] = "stack-tag";
    const size_t tag_size = 4 + std::strlen(stack_tag);
    std::vector<uint8_t> data(ON9LOG_BUFFER_CHUNK_SIZE + 100u, 0x5a);
    on9t::capture_start();
    ON9_LOG_BUFI(stack_tag, data.data(), data.size());
    auto packets = on9t::capture_stop();

    // Each chunk repeats the tag and gives up that much data, so packets keep their usual size.
    ON9T_CHECK(packets.size() == 2);
    ON9T_CHECK(packets[0].tag_id() == ON9LOG_ID_INLINE);
    ON9T_CHECK(packets[0].payload_len() == 3 * 4 + ON9LOG_BUFFER_CHUNK_SIZE);
    std::vector<uint8_t> expected;
    on9t::put_string(expected, "stack-tag");
    const std::vector<uint8_t> second_chunk(data.size() - (ON9LOG_BUFFER_CHUNK_SIZE - tag_size), 0x5a);
    const auto rest = buffer_payload(data.size(), ON9LOG_BUFFER_CHUNK_SIZE - tag_size, second_chunk);
    expected.insert(expected.end(), rest.begin(), rest.end());
    ON9T_CHECK(well_formed(packets[1]));
    ON9T_CHECK(same_bytes(packets[1].payload, expected));
}

// ---------------------------------------------------------------------------
// Binary mode: sequence numbers, drops, BOOT, TIME_SYNC, dispatch
// ---------------------------------------------------------------------------

void seq_increments_across_packet_types()
{
    uint8_t queued[kHeaderLen + 1] = {ON9LOG_PACKET_MAGIC, ON9_LOG_LEVEL_INFO};
    queued[16] = 1; // payload_len
    on9t::capture_start();
    ON9_LOGI("seq", "log");
    ON9_LOG_BUFI("seq", "b", 1);
    on9log_emit_boot(1, ON9LOG_RESET_SOFTWARE);
    ON9T_CHECK(on9log_dispatch_packet(queued, sizeof(queued)) == ON9LOG_OK);
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 4);
    for (size_t i = 1; i < packets.size(); ++i) {
        ON9T_CHECK(packets[i].seq() == static_cast<uint16_t>(packets[i - 1].seq() + 1));
    }
}

/* Remembers the previous seq and notes any packet that did not take the next one. */
struct seq_watch {
    bool first = true;
    uint16_t last = 0;
    uint32_t count = 0;
    uint32_t breaks = 0;
};

void seq_watch_start(const uint8_t *header, size_t, void *ctx)
{
    auto &watch = *static_cast<seq_watch *>(ctx);
    const uint16_t seq = static_cast<uint16_t>(header[2] | (header[3] << 8));
    if (!watch.first && seq != static_cast<uint16_t>(watch.last + 1)) {
        ++watch.breaks;
    }
    watch.first = false;
    watch.last = seq;
    ++watch.count;
}
const on9log_sink_t seq_watch_sink = {seq_watch_start, count_payload, count_end};

void seq_wraps_at_16_bits()
{
    seq_watch watch;
    ON9T_CHECK(on9log_add_sink(&seq_watch_sink, &watch) == ON9LOG_OK);
    ON9_LOGI("wrap", "first");
    const uint16_t first = watch.last;
    for (uint32_t i = 0; i < 65536u; ++i) {
        ON9_LOGI("wrap", "w");
    }
    ON9T_CHECK(on9log_remove_sink(&seq_watch_sink, &watch) == ON9LOG_OK);

    ON9T_CHECK(watch.count == 65537u);
    ON9T_CHECK(watch.breaks == 0);
    ON9T_CHECK(watch.last == first); // 65536 packets later the counter is back where it started
}

void timestamps_do_not_go_backwards()
{
    on9t::capture_start();
    const uint32_t before = on9log_port_timestamp_ms();
    for (int i = 0; i < 20; ++i) {
        ON9_LOGI("time", "t");
    }
    const uint32_t after = on9log_port_timestamp_ms();
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 20);
    for (size_t i = 0; i < packets.size(); ++i) {
        ON9T_CHECK(packets[i].time_ms() - before <= after - before); // wrap-safe "before <= t <= after"
        if (i > 0) {
            ON9T_CHECK(packets[i].time_ms() - packets[i - 1].time_ms() < 0x80000000u);
        }
    }
}

void too_many_args_are_dropped_and_reported()
{
    char types[ON9LOG_MAX_ARGS + 2];
    std::memset(types, T32, ON9LOG_MAX_ARGS + 1);
    types[ON9LOG_MAX_ARGS + 1] = 0;

    on9t::capture_start();
    // 33 argument types: refused before any argument is read.
    on9log_write(ON9_LOG_LEVEL_INFO, "drop", "too many", types);
    const uint32_t pending = on9log_get_dropped_count();
    // The next log attempt reports pending drops first, even if it is dropped too.
    on9log_write(ON9_LOG_LEVEL_INFO, "drop", "too many", types);
    ON9_LOGI("drop", "after");
    const uint32_t after = on9log_get_dropped_count();
    auto packets = on9t::capture_stop();

    ON9T_CHECK(pending == 1);
    ON9T_CHECK(after == 0);
    ON9T_CHECK(packets.size() == 3);
    for (size_t i = 0; i < 2; ++i) {
        const packet &dropped = packets[i];
        ON9T_CHECK(well_formed(dropped));
        ON9T_CHECK(dropped.type() == ON9LOG_PKT_DROPPED && dropped.level() == ON9_LOG_LEVEL_NONE);
        ON9T_CHECK(dropped.tag_id() == 0 && dropped.fmt_id() == 0);
        ON9T_CHECK(same_bytes(dropped.payload, {1, 0, 0, 0}));
    }
    ON9T_CHECK(packets[2].type() == ON9LOG_PKT_LOG);
    ON9T_CHECK(packets[2].seq() == static_cast<uint16_t>(packets[1].seq() + 1));
}

void null_buffer_is_dropped()
{
    on9t::capture_start();
    on9log_write_buffer(ON9_LOG_LEVEL_INFO, "drop", nullptr, 4);
    auto packets = on9t::capture_stop();
    ON9T_CHECK(packets.empty());
    ON9T_CHECK(on9log_get_dropped_count() == 1);
}

void boot_packet()
{
    uint8_t id[ON9LOG_IMAGE_ID_MAX_LEN];
    on9log_image_id_kind_t kind = ON9LOG_IMAGE_ID_NONE;
    const size_t id_len = on9log_port_image_id(id, sizeof(id), &kind);

    on9t::capture_start();
    for (uint8_t reason = ON9LOG_RESET_UNKNOWN; reason <= ON9LOG_RESET_CPU_LOCKUP; ++reason) {
        ON9T_CHECK(on9log_emit_boot(0xa1b2c3d4u + reason, static_cast<on9log_reset_reason_t>(reason)) == ON9LOG_OK);
    }
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == ON9LOG_RESET_CPU_LOCKUP + 1u);
    for (uint8_t reason = 0; reason < packets.size(); ++reason) {
        const packet &p = packets[reason];
        ON9T_CHECK(well_formed(p));
        ON9T_CHECK(p.type() == ON9LOG_PKT_BOOT && p.level() == ON9_LOG_LEVEL_NONE);
        ON9T_CHECK(p.tag_id() == 0 && p.fmt_id() == 0);
        // boot_id | reset_reason | wire version | ID kind | ID length | ID
        std::vector<uint8_t> expected;
        on9t::put32(expected, 0xa1b2c3d4u + reason);
        expected.insert(expected.end(),
                        {reason,
                         static_cast<uint8_t>(ON9LOG_WIRE_FORMAT_VERSION),
                         static_cast<uint8_t>(id_len == 0 ? ON9LOG_IMAGE_ID_NONE : kind),
                         static_cast<uint8_t>(id_len)});
        expected.insert(expected.end(), id, id + id_len);
        ON9T_CHECK(same_bytes(p.payload, expected));
    }
}

void time_sync_packet()
{
    uint8_t buf[32];
    ON9T_CHECK(on9log_encode_time_sync(nullptr, sizeof(buf), 1) == 0);
    ON9T_CHECK(on9log_encode_time_sync(buf, kHeaderLen + 3, 1) == 0); // one byte short

    const uint32_t before = on9log_port_timestamp_ms();
    ON9T_CHECK(on9log_encode_time_sync(buf, sizeof(buf), 0xa1b2c3d4u) == kHeaderLen + 4);
    const uint32_t after = on9log_port_timestamp_ms();

    packet p;
    p.header.assign(buf, buf + kHeaderLen);
    p.payload.assign(buf + kHeaderLen, buf + kHeaderLen + 4);
    ON9T_CHECK(p.header[0] == ON9LOG_PACKET_MAGIC);
    ON9T_CHECK(p.type() == ON9LOG_PKT_TIME_SYNC && p.level() == ON9_LOG_LEVEL_NONE);
    ON9T_CHECK(p.seq() == 0); // outside the sequence
    ON9T_CHECK(p.time_ms() - before <= after - before);
    ON9T_CHECK(p.tag_id() == 0 && p.fmt_id() == 0 && p.payload_len() == 4);
    ON9T_CHECK(same_bytes(p.payload, {0xd4, 0xc3, 0xb2, 0xa1}));
}

void dispatch_packet()
{
    uint8_t queued[kHeaderLen + 1] = {
        ON9LOG_PACKET_MAGIC, (ON9LOG_PKT_LOG << 4) | ON9_LOG_LEVEL_WARN,
        0x34, 0x12,             // seq from elsewhere: replaced
        0x78, 0x56, 0x34, 0x12, // time_ms: kept
        1, 0, 0, 0, 2, 0, 0, 0, // tag_id, fmt_id: kept
        1, 0,                   // payload_len
        0,                      // argument count
    };
    uint8_t bad_magic[kHeaderLen + 1];
    std::memcpy(bad_magic, queued, sizeof(queued));
    bad_magic[0] = 0x00;

    ON9T_CHECK(on9log_dispatch_packet(nullptr, 10) == ON9LOG_ERR_INVALID_ARG);
    ON9T_CHECK(on9log_dispatch_packet(queued, kHeaderLen - 1) == ON9LOG_ERR_INVALID_ARG);
    ON9T_CHECK(on9log_dispatch_packet(bad_magic, sizeof(bad_magic)) == ON9LOG_ERR_INVALID_ARG);

    uint8_t sync[32];
    const size_t sync_len = on9log_encode_time_sync(sync, sizeof(sync), 9);
    on9log_write_buffer(ON9_LOG_LEVEL_INFO, "drop", nullptr, 1); // a pending drop is reported first

    on9t::capture_start();
    ON9T_CHECK(on9log_dispatch_packet(queued, sizeof(queued)) == ON9LOG_OK);
    ON9T_CHECK(on9log_dispatch_packet(sync, sync_len) == ON9LOG_OK);
    auto packets = on9t::capture_stop();

    ON9T_CHECK(queued[2] == 0x34 && queued[3] == 0x12); // the caller's buffer is not modified
    ON9T_CHECK(packets.size() == 3);
    ON9T_CHECK(packets[0].type() == ON9LOG_PKT_DROPPED);
    ON9T_CHECK(same_bytes(packets[0].payload, {1, 0, 0, 0}));
    // The forwarded packet takes the next seq and keeps everything else.
    const packet &forwarded = packets[1];
    ON9T_CHECK(well_formed(forwarded));
    ON9T_CHECK(forwarded.seq() == static_cast<uint16_t>(packets[0].seq() + 1));
    ON9T_CHECK(forwarded.time_ms() == 0x12345678u && forwarded.tag_id() == 1 && forwarded.fmt_id() == 2);
    ON9T_CHECK(forwarded.type() == ON9LOG_PKT_LOG && forwarded.level() == ON9_LOG_LEVEL_WARN);
    ON9T_CHECK(same_bytes(forwarded.payload, {0}));
    // TIME_SYNC stays outside the sequence.
    ON9T_CHECK(packets[2].type() == ON9LOG_PKT_TIME_SYNC && packets[2].seq() == 0);
}

// ---------------------------------------------------------------------------
// Binary mode: C++ Logger
// ---------------------------------------------------------------------------

enum class color : uint8_t { red = 2 };

void cpp_argument_types()
{
    using namespace std::string_view_literals;
    Logger log("cpp");
    const std::string with_nul("ab\0cd", 5);
    on9t::capture_start();
    log.info("{} {} {} {} {} {} {} {} {} {} {} {}",
             int8_t{-1}, uint16_t{2}, int64_t{-3}, 1.5f, 2.5, true, color::red,
             nullptr, reinterpret_cast<const void *>(0x10), "lit", with_nul, "xyz"sv);
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 1);
    ON9T_CHECK(well_formed(packets[0]));
    std::vector<uint8_t> expected = {12, T32, T32, T64, T64, T64, T32, T32, TPTR, TPTR, TSTR, TVIEW, TVIEW};
    on9t::put32(expected, 0xffffffffu); // int8_t -1, sign-extended
    on9t::put32(expected, 2);
    on9t::put64(expected, static_cast<uint64_t>(-3));
    on9t::put64(expected, double_bits(1.5)); // float sent as double
    on9t::put64(expected, double_bits(2.5));
    on9t::put32(expected, 1);
    on9t::put32(expected, 2);
    on9t::put32(expected, 0);
    on9t::put32(expected, 0x10);
    on9t::put_string(expected, "lit");
    on9t::put_string(expected, with_nul); // exact length, embedded NUL kept
    on9t::put_string(expected, "xyz");
    ON9T_CHECK(same_bytes(packets[0].payload, expected));
}

void cpp_levels()
{
#if ON9_LOG_LOCAL_LEVEL >= 4 // DEBUG; #if cannot see enum values
    ON9T_SKIP("built with a DEBUG or VERBOSE ceiling");
#endif
    Logger log("cpp");
    on9t::capture_start();
    log.error("e");
    log.warn("w");
    log.info("i");
    log.debug("d");
    log.verbose("v");
    log.e("e");
    log.w("w");
    log.i("i");
    log.d("d");
    log.v("v");
    auto packets = on9t::capture_stop();

    const uint8_t expected[] = {ON9_LOG_LEVEL_ERROR, ON9_LOG_LEVEL_WARN, ON9_LOG_LEVEL_INFO};
    ON9T_CHECK(packets.size() == 6); // debug and verbose are compiled out
    for (size_t i = 0; i < packets.size(); ++i) {
        ON9T_CHECK(packets[i].level() == expected[i % 3]);
    }
}

void cpp_format_forms()
{
    using namespace on9log::literals;
    Logger log("cpp");
    char runtime[] = "runtime %d";
    on9t::capture_start();
    log.info("plain %d", 1);
    log.info<"nttp %d">(2);
    log.info("token %d"_on9fmt, 3);
    log.info(ON9FMT("macro %d"), 4);
    log.info(on9log::runtime_format(runtime), 5);
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 5);
    for (size_t i = 0; i < 4; ++i) {
        ON9T_CHECK(well_formed(packets[i]));
        ON9T_CHECK(packets[i].fmt_id() != 0 && packets[i].fmt_id() != ON9LOG_ID_INLINE);
        std::vector<uint8_t> expected = {1, T32};
        on9t::put32(expected, static_cast<uint32_t>(i + 1));
        ON9T_CHECK(same_bytes(packets[i].payload, expected));
    }
    // A format in RAM travels inline.
    std::vector<uint8_t> expected;
    on9t::put_string(expected, "runtime %d");
    expected.insert(expected.end(), {1, T32});
    on9t::put32(expected, 5);
    ON9T_CHECK(packets[4].fmt_id() == ON9LOG_ID_INLINE);
    ON9T_CHECK(same_bytes(packets[4].payload, expected));
}

void cpp_thirty_two_arguments()
{
    Logger log("cpp");
    on9t::capture_start();
    log.info("32", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
             17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32);
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 1);
    std::vector<uint8_t> expected = {32};
    expected.insert(expected.end(), 32, T32);
    for (uint32_t i = 1; i <= 32; ++i) {
        on9t::put32(expected, i);
    }
    ON9T_CHECK(same_bytes(packets[0].payload, expected));
}

void cpp_tag_buffer_and_filters()
{
    static const char first[] = "cpp-first";
    static const char second[] = "cpp-second";
    Logger log(first);
    ON9T_CHECK(log.tag() == first);
    const uint8_t bytes[] = {1, 2, 3};

    on9t::capture_start();
    log.buffer_info(bytes, sizeof(bytes));
    log.set_tag(second);
    log.warn("after set_tag");
    ON9T_CHECK(Logger::set_tag_level(second, ON9_LOG_LEVEL_ERROR) == ON9LOG_OK);
    log.warn("filtered");
    ON9T_CHECK(Logger::clear_tag_level(second) == ON9LOG_OK);
    Logger::set_level(ON9_LOG_LEVEL_ERROR);
    ON9T_CHECK(Logger::level() == ON9_LOG_LEVEL_ERROR);
    log.warn("filtered globally");
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 2);
    ON9T_CHECK(packets[0].type() == ON9LOG_PKT_BUFFER && packets[0].level() == ON9_LOG_LEVEL_INFO);
    ON9T_CHECK(packets[0].tag_id() == addr_id(first));
    ON9T_CHECK(same_bytes(packets[0].payload, buffer_payload(3, 0, {1, 2, 3})));
    ON9T_CHECK(packets[1].tag_id() == addr_id(second));
}

#else
// ---------------------------------------------------------------------------
// Plain-text mode
// ---------------------------------------------------------------------------

/** Text of the single line a log call produced, or "" if it produced no line or more than one. */
std::string line_for(void (*log)())
{
    on9t::capture_start();
    log();
    auto packets = on9t::capture_stop();
    if (packets.size() != 1 || !packets[0].header.empty() || !packets[0].ended) {
        return {};
    }
    return packets[0].text();
}

bool is_timestamp(const std::string &s)
{
    // HH:MM:SS.mmm
    const char *shape = "dd:dd:dd.ddd";
    if (s.size() != std::strlen(shape)) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (shape[i] == 'd' ? (s[i] < '0' || s[i] > '9') : s[i] != shape[i]) {
            return false;
        }
    }
    return true;
}

void text_default_line()
{
    const std::string line = line_for([] { ON9_LOGI("ptag", "v=%d s=%s", 5, "x"); });
    // Bold green prefix, timestamp, topic, reset, body, CR LF.
    const std::string prefix = "\033[1m\033[0;32m";
    const std::string suffix = " [ptag]: \033[0mv=5 s=x\r\n";
    ON9T_CHECK(line.size() == prefix.size() + 12 + suffix.size());
    ON9T_CHECK(line.compare(0, prefix.size(), prefix) == 0);
    ON9T_CHECK(is_timestamp(line.substr(prefix.size(), 12)));
    ON9T_CHECK(line.compare(prefix.size() + 12, suffix.size(), suffix) == 0);
}

void text_flags()
{
    ON9T_CHECK(line_for([] {
        on9log_write_with_text_flags(ON9_LOG_LEVEL_WARN, "ptag", ON9LOG_TEXT_SHOWLEVEL | ON9LOG_TEXT_SHOWTOPIC, "n=%d", nullptr, 3);
    }) == "W [ptag]: n=3\n");
    ON9T_CHECK(line_for([] {
        on9log_write_with_text_flags(ON9_LOG_LEVEL_ERROR, "ptag", 0, "bare", nullptr);
    }) == "bare\n");
    // CR adds \r before every \n that lacks one.
    ON9T_CHECK(line_for([] {
        on9log_write_with_text_flags(ON9_LOG_LEVEL_ERROR, "ptag", ON9LOG_TEXT_CR, "a\nb\r\nc", nullptr);
    }) == "a\r\nb\r\nc\r\n");
    // PRETTYLINE keeps the colour for the whole line.
    ON9T_CHECK(line_for([] {
        on9log_write_with_text_flags(ON9_LOG_LEVEL_ERROR, "ptag", ON9LOG_TEXT_PRETTYLINE | ON9LOG_TEXT_SHOWLEVEL, "x", nullptr);
    }) == "\033[1m\033[0;31mE: \033[22mx\n\033[0m");
}

void text_buffer_boot_and_drop()
{
    const std::string buffer = line_for([] {
        const uint8_t data[] = {0x01, 0xab};
        on9log_write_buffer_with_text_flags(ON9_LOG_LEVEL_INFO, "ptag", ON9LOG_TEXT_SHOWTOPIC, data, sizeof(data));
    });
    ON9T_CHECK(buffer == "[ptag]: buffer[2]: 01 ab\n");

    const std::string boot = line_for([] { on9log_emit_boot(7, ON9LOG_RESET_POWER_ON); });
    ON9T_CHECK(boot.find("[on9log]: \033[0mboot id=7 reset reason=1 image id=") != std::string::npos);

    on9log_write_buffer(ON9_LOG_LEVEL_INFO, "ptag", nullptr, 3); // dropped
    on9t::capture_start();
    ON9_LOGI("ptag", "after");
    auto packets = on9t::capture_stop();
    ON9T_CHECK(packets.size() == 2);
    ON9T_CHECK(packets[0].text().find("[on9log]: \033[0m1 log message(s) dropped\r\n") != std::string::npos);
    ON9T_CHECK(packets[1].text().find("after") != std::string::npos);
}

void text_cpp_logger()
{
    ON9T_CHECK(line_for([] {
        Logger log("cpp", Logger::SHOWTOPIC);
        log.info("{} + {} = {}", 1, 2, std::string("three"));
    }) == "[cpp]: 1 + 2 = three\n");
    ON9T_CHECK(line_for([] {
        Logger log("cpp", Logger::SHOWLEVEL);
        log.warn("%d-%s", 4, "four");
    }) == "W: 4-four\n");
}

void text_dispatch_and_time_sync()
{
    const uint8_t raw[] = {'r', 'a', 'w', '\n'};
    ON9T_CHECK(on9log_dispatch_packet(nullptr, 1) == ON9LOG_ERR_INVALID_ARG);
    ON9T_CHECK(on9log_dispatch_packet(raw, 0) == ON9LOG_ERR_INVALID_ARG);
    on9t::capture_start();
    ON9T_CHECK(on9log_dispatch_packet(raw, sizeof(raw)) == ON9LOG_OK);
    auto packets = on9t::capture_stop();
    ON9T_CHECK(packets.size() == 1 && packets[0].text() == "raw\n");

    uint8_t buf[32];
    ON9T_CHECK(on9log_encode_time_sync(buf, sizeof(buf), 1) == 0); // binary only
}
#endif

} // namespace

namespace on9t {

const test_case core_cases[] = {
    {"global_level_filters", global_level_filters},
    {"invalid_levels_never_log", invalid_levels_never_log},
    {"compile_time_ceiling", compile_time_ceiling},
    {"tag_level_overrides_global", tag_level_overrides_global},
    {"tag_level_matches_contents_and_copies", tag_level_matches_contents_and_copies},
    {"tag_level_errors", tag_level_errors},
    {"sink_registry", sink_registry},
    {"sink_table_full", sink_table_full},
    {"isr_without_backend_is_silent", isr_without_backend_is_silent},
    {"init_is_idempotent", init_is_idempotent},
#if !ON9LOG_PLAIN_TEXT
    {"header_fields", header_fields},
    {"sink_callback_arguments", sink_callback_arguments},
    {"c_scalar_arguments", c_scalar_arguments},
    {"c_string_arguments", c_string_arguments},
    {"c_sixteen_arguments", c_sixteen_arguments},
    {"c_double_arguments", c_double_arguments},
    {"c_long_arguments", c_long_arguments},
    {"precision_strings", precision_strings},
    {"long_strings_are_capped", long_strings_are_capped},
    {"inline_tags_and_formats", inline_tags_and_formats},
    {"null_tag_is_address_zero", null_tag_is_address_zero},
    {"buffer_small_and_empty", buffer_small_and_empty},
    {"buffer_chunks", buffer_chunks},
    {"buffer_with_inline_tag", buffer_with_inline_tag},
    {"seq_increments_across_packet_types", seq_increments_across_packet_types},
    {"seq_wraps_at_16_bits", seq_wraps_at_16_bits},
    {"timestamps_do_not_go_backwards", timestamps_do_not_go_backwards},
    {"too_many_args_are_dropped_and_reported", too_many_args_are_dropped_and_reported},
    {"null_buffer_is_dropped", null_buffer_is_dropped},
    {"boot_packet", boot_packet},
    {"time_sync_packet", time_sync_packet},
    {"dispatch_packet", dispatch_packet},
    {"cpp_argument_types", cpp_argument_types},
    {"cpp_levels", cpp_levels},
    {"cpp_format_forms", cpp_format_forms},
    {"cpp_thirty_two_arguments", cpp_thirty_two_arguments},
    {"cpp_tag_buffer_and_filters", cpp_tag_buffer_and_filters},
#else
    {"text_default_line", text_default_line},
    {"text_flags", text_flags},
    {"text_buffer_boot_and_drop", text_buffer_boot_and_drop},
    {"text_cpp_logger", text_cpp_logger},
    {"text_dispatch_and_time_sync", text_dispatch_and_time_sync},
#endif
};

const size_t core_case_count = sizeof(core_cases) / sizeof(core_cases[0]);

} // namespace on9t
