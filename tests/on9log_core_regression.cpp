#include "on9log.h"
#include "on9log_fmt.h"

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
};

static std::vector<captured_packet> g_captured;

static void capture_start(const uint8_t *header, size_t header_len, void *)
{
    g_captured.push_back({std::vector<uint8_t>(header, header + header_len), 0});
}

static void capture_payload(const uint8_t *, size_t len, size_t, size_t, void *)
{
    g_captured.back().payload_bytes += len;
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
    check_boot_and_time_sync();
#endif
    return 0;
}
