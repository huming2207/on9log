#include "on9log.h"
#include "on9log_fmt.h"
#include "on9log_port.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

static void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

struct blocking_sink_state {
    std::mutex mutex;
    std::condition_variable cv;
    bool block = false;
    bool entered = false;
    bool release = false;
    std::atomic<unsigned> starts{0};
};

static void blocking_start(const uint8_t *, size_t, void *ctx)
{
    auto &state = *static_cast<blocking_sink_state *>(ctx);
    state.starts.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(state.mutex);
    if (!state.block) {
        return;
    }
    state.entered = true;
    state.cv.notify_all();
    state.cv.wait(lock, [&] { return state.release; });
}

static void blocking_payload(const uint8_t *, size_t, size_t, size_t, void *) {}
static void blocking_end(void *) {}

static const on9log_sink_t blocking_sink = {
    .start_cb = blocking_start,
    .payload_cb = blocking_payload,
    .end_cb = blocking_end,
};

// Collects what a sink receives for each packet: the header and the payload bytes after it.
struct captured_packet {
    std::vector<uint8_t> header;
    size_t payload_bytes = 0;
    std::vector<uint8_t> payload;
};

static std::vector<captured_packet> g_captured;

static void capture_start(const uint8_t *header, size_t header_len, void *)
{
    g_captured.push_back({std::vector<uint8_t>(header, header + header_len), 0, {}});
}

static void capture_payload(const uint8_t *payload, size_t len, size_t, size_t, void *)
{
    g_captured.back().payload_bytes += len;
    g_captured.back().payload.insert(g_captured.back().payload.end(), payload, payload + len);
}

static void capture_end(void *) {}

static const on9log_sink_t capture_sink = {
    .start_cb = capture_start,
    .payload_cb = capture_payload,
    .end_cb = capture_end,
};

static uint16_t header_payload_len(const captured_packet &packet)
{
    require(packet.header.size() == sizeof(on9log_packet_header_t));
    return static_cast<uint16_t>(packet.header[16] | (packet.header[17] << 8));
}

#if !ON9LOG_PLAIN_TEXT
// Every packet's header must state exactly how many payload bytes follow it.
static void check_payload_lengths()
{
    require(on9log_add_sink(&capture_sink, nullptr) == ON9LOG_OK);

    char text[] = "dynamic";
    const char *null_text = nullptr;
    ON9_LOGI("lengths", "no arguments");
    ON9_LOGI("lengths", "ints %d %u", 1, 2u);
    ON9_LOGI("lengths", "wide %lld", 1234567890123ll);
    ON9_LOGI("lengths", "string %s", text);
    ON9_LOGI("lengths", "null string %s and ptr %p", null_text, ON9_PTR(text));
    ON9_LOGI("lengths", "precision %.*s", 3, text);
    ON9_LOG_BUFI("lengths", text, sizeof(text));

    require(g_captured.size() == 7);
    for (const auto &packet : g_captured) {
        require(header_payload_len(packet) != ON9LOG_PAYLOAD_LEN_STREAMING);
        require(header_payload_len(packet) == packet.payload_bytes);
    }

    require(on9log_remove_sink(&capture_sink, nullptr) == ON9LOG_OK);
    g_captured.clear();
}
#endif

#if !ON9LOG_PLAIN_TEXT
static uint32_t read_u32(const std::vector<uint8_t> &bytes, size_t offset)
{
    require(offset + 4 <= bytes.size());
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

// Checks a length-prefixed string at offset and returns the offset just past it.
static size_t require_string(const std::vector<uint8_t> &bytes, size_t offset, const char *expected)
{
    const size_t len = std::strlen(expected);
    require(read_u32(bytes, offset) == len);
    require(offset + 4 + len <= bytes.size());
    require(std::memcmp(&bytes[offset + 4], expected, len) == 0);
    return offset + 4 + len;
}

// Tags and formats the host cannot resolve by address travel inside the packet.
static void check_inline_strings()
{
    char stack_tag[] = "stack-tag";
    char runtime_format[] = "runtime %d";
    require(!on9log_port_addr_in_image(stack_tag));
    require(!on9log_port_addr_in_image(runtime_format));
#if defined(__linux__)
    // The test is linked without PIE, so literals resolve by address.
    require(on9log_port_addr_in_image("literal"));
#endif

    require(on9log_add_sink(&capture_sink, nullptr) == ON9LOG_OK);
    ON9_LOGI(stack_tag, "value %d", 7);
    ON9_LOG_RUNTIMEI("literal", runtime_format, 8);
    ON9_LOG_RUNTIMEI(stack_tag, runtime_format, 9);
    ON9_LOG_BUFI(stack_tag, runtime_format, 4);
    ON9_LOGI("literal", "by address %d", 10);
    std::vector<uint8_t> big(ON9LOG_BUFFER_CHUNK_SIZE + 100u, 0x5a);
    ON9_LOG_BUFI(stack_tag, big.data(), big.size());
    require(on9log_remove_sink(&capture_sink, nullptr) == ON9LOG_OK);

    require(g_captured.size() == 7);
    for (const auto &packet : g_captured) {
        require(header_payload_len(packet) == packet.payload.size());
    }

    // Inline tag: the payload starts with it, then the argument count.
    const auto &tag_only = g_captured[0];
    require(read_u32(tag_only.header, 8) == ON9LOG_ID_INLINE);
    require(tag_only.payload[require_string(tag_only.payload, 0, "stack-tag")] == 1);
#if defined(__linux__)
    require(read_u32(tag_only.header, 12) != ON9LOG_ID_INLINE);
#endif

    // Inline runtime format with a tag sent by address.
    const auto &format_only = g_captured[1];
    require(read_u32(format_only.header, 12) == ON9LOG_ID_INLINE);
#if defined(__linux__)
    require(read_u32(format_only.header, 8) != ON9LOG_ID_INLINE);
#endif
    require(format_only.payload[require_string(format_only.payload, 0, "runtime %d")] == 1);

    // Both inline: tag first, then format.
    const auto &both = g_captured[2];
    require(read_u32(both.header, 8) == ON9LOG_ID_INLINE && read_u32(both.header, 12) == ON9LOG_ID_INLINE);
    size_t offset = require_string(both.payload, 0, "stack-tag");
    offset = require_string(both.payload, offset, "runtime %d");
    require(both.payload[offset] == 1 && read_u32(both.payload, offset + 2) == 9);

    // Buffer: inline tag, then total_len, offset, chunk_len, data.
    const auto &buffer = g_captured[3];
    require((buffer.header[1] >> 4) == ON9LOG_PKT_BUFFER);
    require(read_u32(buffer.header, 8) == ON9LOG_ID_INLINE);
    offset = require_string(buffer.payload, 0, "stack-tag");
    require(read_u32(buffer.payload, offset) == 4 && read_u32(buffer.payload, offset + 4) == 0 &&
            read_u32(buffer.payload, offset + 8) == 4);
    require(std::memcmp(&buffer.payload[offset + 12], "runt", 4) == 0);

#if defined(__linux__)
    const auto &by_address = g_captured[4];
    require(read_u32(by_address.header, 8) != ON9LOG_ID_INLINE && read_u32(by_address.header, 12) != ON9LOG_ID_INLINE);
#endif

    // A chunked buffer with an inline tag keeps each packet at the usual size.
    require(header_payload_len(g_captured[5]) == 3u * sizeof(uint32_t) + ON9LOG_BUFFER_CHUNK_SIZE);
    offset = require_string(g_captured[5].payload, 0, "stack-tag");
    const uint32_t first_chunk = read_u32(g_captured[5].payload, offset + 8);
    offset = require_string(g_captured[6].payload, 0, "stack-tag");
    require(first_chunk + read_u32(g_captured[6].payload, offset + 8) == big.size());

    g_captured.clear();
}

static void check_boot_and_time_sync()
{
    require(on9log_add_sink(&capture_sink, nullptr) == ON9LOG_OK);
    require(on9log_emit_boot(0xa1b2c3d4u, ON9LOG_RESET_BROWNOUT) == ON9LOG_OK);
    require(on9log_remove_sink(&capture_sink, nullptr) == ON9LOG_OK);

    require(g_captured.size() == 1);
    const auto &boot = g_captured[0];
    require((boot.header[1] >> 4) == ON9LOG_PKT_BOOT);
    require(header_payload_len(boot) == 5 && boot.payload_bytes == 5);
    g_captured.clear();

    uint8_t sync[32];
    require(on9log_encode_time_sync(sync, 21, 1) == 0);  // too small
    require(on9log_encode_time_sync(sync, sizeof(sync), 0xa1b2c3d4u) == 22);
    require(sync[0] == ON9LOG_PACKET_MAGIC);
    require((sync[1] >> 4) == ON9LOG_PKT_TIME_SYNC);
    require(sync[16] == 4 && sync[17] == 0);
    require(sync[18] == 0xd4 && sync[19] == 0xc3 && sync[20] == 0xb2 && sync[21] == 0xa1);
}
#endif

int main()
{
    require(on9log_init() == ON9LOG_OK);

    blocking_sink_state state;
    require(on9log_add_sink(&blocking_sink, &state) == ON9LOG_OK);

    int side_effect = 1;
    ON9_LOGI("regression", "side effect=%d", side_effect++);
    require(side_effect == 2);

    const char *runtime_format = "runtime value=%d";
    ON9_LOG_RUNTIMEI("regression", runtime_format, 42);

    char owned_tag[] = "owned-tag";
    on9log_set_level(ON9_LOG_LEVEL_NONE);
    require(on9log_set_tag_level(owned_tag, ON9_LOG_LEVEL_INFO) == ON9LOG_OK);
    std::memset(owned_tag, 'x', sizeof(owned_tag) - 1u);
    unsigned starts_before = state.starts.load(std::memory_order_relaxed);
    ON9_LOGI("owned-tag", "owned tag still matches");
    require(state.starts.load(std::memory_order_relaxed) == starts_before + 1u);
    require(on9log_clear_tag_level("owned-tag") == ON9LOG_OK);

    std::atomic<bool> stop_filter_stress{false};
    std::thread filter_reader([&] {
        while (!stop_filter_stress.load(std::memory_order_acquire)) {
            ON9_LOGI("reuse-a", "filter reader a");
            ON9_LOGI("reuse-b", "filter reader b");
        }
    });
    for (unsigned i = 0; i < 200u; ++i) {
        char transient_tag[] = "reuse-a";
        const char *stable_tag = "reuse-a";
        if ((i & 1u) != 0u) {
            transient_tag[6] = 'b';
            stable_tag = "reuse-b";
        }
        require(on9log_set_tag_level(transient_tag, ON9_LOG_LEVEL_INFO) == ON9LOG_OK);
        std::memset(transient_tag, 'x', sizeof(transient_tag) - 1u);
        require(on9log_clear_tag_level(stable_tag) == ON9LOG_OK);
    }
    stop_filter_stress.store(true, std::memory_order_release);
    filter_reader.join();
    on9log_set_level(ON9_LOG_LEVEL_VERBOSE);

    {
        std::lock_guard lock(state.mutex);
        state.block = true;
        state.entered = false;
        state.release = false;
    }

    std::thread logger([] { ON9_LOGI("regression", "blocked dispatch"); });
    {
        std::unique_lock lock(state.mutex);
        state.cv.wait(lock, [&] { return state.entered; });
    }

    std::atomic<bool> removal_started{false};
    std::atomic<bool> removal_done{false};
    std::thread remover([&] {
        removal_started.store(true, std::memory_order_release);
        require(on9log_remove_sink(&blocking_sink, &state) == ON9LOG_OK);
        removal_done.store(true, std::memory_order_release);
    });
    while (!removal_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(!removal_done.load(std::memory_order_acquire));

    {
        std::lock_guard lock(state.mutex);
        state.release = true;
    }
    state.cv.notify_all();
    logger.join();
    remover.join();
    require(removal_done.load(std::memory_order_acquire));

#if !ON9LOG_PLAIN_TEXT
    check_payload_lengths();
    check_inline_strings();
    check_boot_and_time_sync();
#endif
    return 0;
}
