/*
 * on9log tests on ESP32-S3, run in QEMU: the shared core cases, then cases for
 * the ESP-IDF port. Prints one line per case and a final "ON9T RESULT" line,
 * then restarts; ci/run_tests.sh runs QEMU with -no-reboot, so that ends it.
 */

#include "on9log_test.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_stdio_log_vfs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "on9log.hpp"
#include "on9log_esp_isr.h"
#include "on9log_port.h"

/*
 * QEMU only (linked with -Wl,--wrap=rtc_clk_bbpll_add_consumer, see main/CMakeLists.txt).
 * ESP-IDF 6.2-dev's USB-Serial-JTAG connection monitor, linked by vfs, calls this at
 * boot and it spins until the BBPLL reports calibrated, which QEMU never does. The
 * PLL is already running the CPU here and this app never changes clocks or sleeps.
 */
extern "C" void __wrap_rtc_clk_bbpll_add_consumer(void) {}

extern "C" const char _noload_keep_in_elf_start[];
extern "C" const char _noload_keep_in_elf_end[];

namespace {

using on9t::packet;

[[maybe_unused]] bool in_noload(uint32_t id)
{
    return id >= reinterpret_cast<uintptr_t>(_noload_keep_in_elf_start) &&
           id < reinterpret_cast<uintptr_t>(_noload_keep_in_elf_end);
}

/** Waits up to @p ms for the capture sink to finish @p count packets. */
bool wait_for_packets(size_t count, int ms)
{
    for (int waited = 0; on9t::captured_count() < count; waited += 10) {
        if (waited >= ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

void ignore_payload(const uint8_t *, size_t, size_t, size_t, void *) {}
void ignore_end(void *) {}

// ---------------------------------------------------------------------------
// Port hooks
// ---------------------------------------------------------------------------

DRAM_ATTR const char g_dram_const[] = "dram";
char g_data[] = "data";

void addr_in_image()
{
    static const char rodata[] = "rodata";
    char stack[] = "stack";
    std::vector<char> heap(8, 'h');

    // Flash rodata and the ELF-only .noload section resolve by address...
    ON9T_CHECK(on9log_port_addr_in_image(rodata));
    ON9T_CHECK(on9log_port_addr_in_image("literal"));
#if !ON9LOG_PLAIN_TEXT
    const char *noload = ON9_LOG_NOLOAD_STR("noload");
    ON9T_CHECK(on9log_port_addr_in_image(noload));
    ON9T_CHECK(in_noload(reinterpret_cast<uintptr_t>(noload)));
#endif
    // ...RAM does not: the strings go inline.
    ON9T_CHECK(!on9log_port_addr_in_image(stack));
    ON9T_CHECK(!on9log_port_addr_in_image(heap.data()));
    ON9T_CHECK(!on9log_port_addr_in_image(g_dram_const));
    ON9T_CHECK(!on9log_port_addr_in_image(g_data));
}

void image_id_is_elf_sha256()
{
    uint8_t id[ON9LOG_IMAGE_ID_MAX_LEN];
    on9log_image_id_kind_t kind = ON9LOG_IMAGE_ID_NONE;
    const esp_app_desc_t *desc = esp_app_get_description();

    ON9T_CHECK(on9log_port_image_id(id, sizeof(id), &kind) == sizeof(desc->app_elf_sha256));
    ON9T_CHECK(kind == ON9LOG_IMAGE_ID_ELF_SHA256);
    ON9T_CHECK(std::memcmp(id, desc->app_elf_sha256, sizeof(desc->app_elf_sha256)) == 0);
    ON9T_CHECK(on9log_port_image_id(id, sizeof(desc->app_elf_sha256) - 1, &kind) == 0); // too small

    // ci/run_tests.sh compares this with sha256sum of the ELF it built.
    std::printf("ON9T ELF SHA256: ");
    for (uint8_t byte : desc->app_elf_sha256) {
        std::printf("%02x", byte);
    }
    std::printf("\n");
}

void reset_reason()
{
    if (esp_reset_reason() != ESP_RST_POWERON) {
        ON9T_SKIP("QEMU did not report a power-on reset");
    }
    ON9T_CHECK(on9log_port_reset_reason() == ON9LOG_RESET_POWER_ON);
}

// The timestamp comes from esp_timer, so it has 1 ms resolution even with a 10 ms tick.
void timestamps_use_esp_timer()
{
    const uint32_t start = on9log_port_timestamp_ms();
    ON9T_CHECK(static_cast<uint32_t>(esp_timer_get_time() / 1000) - start <= 1);

    const int64_t wait_until = esp_timer_get_time() + 3000;
    while (esp_timer_get_time() < wait_until) {
    }
    const uint32_t elapsed = on9log_port_timestamp_ms() - start;
    ON9T_CHECK(elapsed >= 3 && elapsed <= 5); // a tick-based clock would give 0 or 10
    ON9T_CHECK(on9log_port_isr_timestamp_ms() - start - elapsed <= 1);
}

#if !ON9LOG_PLAIN_TEXT
void formats_live_in_noload()
{
    using namespace on9log::literals;
    Logger log("esp");
    on9t::capture_start();
    ON9_LOGI("esp", "c macro %d", 1);
    log.info<"nttp %d">(2);
    log.info("token %d"_on9fmt, 3);
    log.info(ON9FMT("macro %d"), 4);
    // ci/run_tests.sh looks for this text in the flashed .bin: there only without reflection.
    log.info("on9t plain literal %d", 5);
    log.e("on9t plain short form %d", 6);
    auto packets = on9t::capture_stop();

    ON9T_CHECK(packets.size() == 6);
    for (size_t i = 0; i < 4; ++i) {
        ON9T_CHECK(in_noload(packets[i].fmt_id()));
    }
    for (size_t i = 4; i < 6; ++i) {
#if ON9LOG_CXX_REFLECTION
        // C++26 reflection moved the plain literal to .noload at compile time.
        ON9T_CHECK(in_noload(packets[i].fmt_id()));
#else
        // Without reflection a plain literal stays in flash rodata, still sent by address.
        ON9T_CHECK(!in_noload(packets[i].fmt_id()) && packets[i].fmt_id() != ON9LOG_ID_INLINE);
#endif
    }
}
#endif

// ---------------------------------------------------------------------------
// The emit lock (a FreeRTOS mutex) on two cores
// ---------------------------------------------------------------------------

std::vector<uint16_t> g_order;

void order_start(const uint8_t *header, size_t, void *)
{
    taskYIELD(); // give the other tasks every chance to get between numbering and output
    if (header != nullptr) {
        g_order.push_back(static_cast<uint16_t>(header[2] | (header[3] << 8)));
    }
}
const on9log_sink_t order_sink = {order_start, ignore_payload, ignore_end};

struct order_task_args {
    SemaphoreHandle_t done;
    int id;
};

void order_task(void *arg)
{
    auto *args = static_cast<order_task_args *>(arg);
    for (int i = 0; i < 500; ++i) {
        ON9_LOGI("order", "task %d log %d", args->id, i);
    }
    xSemaphoreGive(args->done);
    vTaskDelete(nullptr);
}

void wire_order_on_two_cores()
{
    if (ON9LOG_PLAIN_TEXT) {
        ON9T_SKIP("binary mode only: text lines have no sequence number");
    }
    constexpr int tasks = 4;
    g_order.clear();
    g_order.reserve(tasks * 500);
    SemaphoreHandle_t done = xSemaphoreCreateCounting(tasks, 0);
    order_task_args args[tasks];
    ON9T_CHECK(on9log_add_sink(&order_sink, nullptr) == ON9LOG_OK);
    for (int t = 0; t < tasks; ++t) {
        args[t] = {done, t};
        xTaskCreatePinnedToCore(order_task, "order", 4096, &args[t], 5, nullptr, t % 2);
    }
    for (int t = 0; t < tasks; ++t) {
        xSemaphoreTake(done, portMAX_DELAY);
    }
    vSemaphoreDelete(done);
    ON9T_CHECK(on9log_remove_sink(&order_sink, nullptr) == ON9LOG_OK);

    ON9T_CHECK(g_order.size() == tasks * 500u);
    for (size_t i = 1; i < g_order.size(); ++i) {
        ON9T_CHECK(g_order[i] == static_cast<uint16_t>(g_order[i - 1] + 1));
    }
}

struct blocking_state {
    SemaphoreHandle_t entered;
    SemaphoreHandle_t release;
    SemaphoreHandle_t logged;
    SemaphoreHandle_t removed;
};

void blocking_start(const uint8_t *, size_t, void *ctx)
{
    auto *state = static_cast<blocking_state *>(ctx);
    xSemaphoreGive(state->entered);
    xSemaphoreTake(state->release, portMAX_DELAY);
}
const on9log_sink_t blocking_sink = {blocking_start, ignore_payload, ignore_end};

void blocked_logger_task(void *arg)
{
    ON9_LOGI("block", "held open");
    xSemaphoreGive(static_cast<blocking_state *>(arg)->logged);
    vTaskDelete(nullptr);
}

void remover_task(void *arg)
{
    auto *state = static_cast<blocking_state *>(arg);
    (void)on9log_remove_sink(&blocking_sink, state);
    xSemaphoreGive(state->removed);
    vTaskDelete(nullptr);
}

// on9log_remove_sink() waits for a packet in flight, so the caller may free the sink afterwards.
void remove_sink_waits_for_packet_in_flight()
{
    blocking_state state = {xSemaphoreCreateBinary(), xSemaphoreCreateBinary(), xSemaphoreCreateBinary(),
                            xSemaphoreCreateBinary()};
    ON9T_CHECK(on9log_add_sink(&blocking_sink, &state) == ON9LOG_OK);
    xTaskCreatePinnedToCore(blocked_logger_task, "logger", 4096, &state, 5, nullptr, 0);
    ON9T_CHECK(xSemaphoreTake(state.entered, pdMS_TO_TICKS(1000)) == pdTRUE);

    xTaskCreatePinnedToCore(remover_task, "remover", 4096, &state, 5, nullptr, 1);
    const bool removed_early = xSemaphoreTake(state.removed, pdMS_TO_TICKS(50)) == pdTRUE;
    xSemaphoreGive(state.release);
    const bool logged = xSemaphoreTake(state.logged, pdMS_TO_TICKS(1000)) == pdTRUE;
    const bool removed = xSemaphoreTake(state.removed, pdMS_TO_TICKS(1000)) == pdTRUE;
    ON9T_CHECK(!removed_early);
    ON9T_CHECK(logged && removed);
}

// ---------------------------------------------------------------------------
// ISR logging through the ringbuffer and drain task
// ---------------------------------------------------------------------------

volatile bool g_ran_in_isr;

void isr_timer_callback(void *)
{
    g_ran_in_isr = xPortInIsrContext();
    on9t_c_isr(42);
}

void isr_logs_reach_sinks()
{
    ON9T_CHECK(on9log_esp_isr_init() == ESP_OK);
    ON9T_CHECK(on9log_esp_isr_init() == ESP_OK); // already installed
    ON9T_CHECK(on9log_port_isr_ready());

    const esp_timer_create_args_t timer_args = {
        .callback = isr_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_ISR,
        .name = "on9t_isr",
        .skip_unhandled_events = false,
    };
    esp_timer_handle_t timer;
    ON9T_CHECK(esp_timer_create(&timer_args, &timer) == ESP_OK);

    on9t::capture_start();
    ON9_LOGI("isr", "before");
    g_ran_in_isr = false;
    ON9T_CHECK(esp_timer_start_once(timer, 1000) == ESP_OK);
    const bool arrived = wait_for_packets(2, 1000);
    auto packets = on9t::capture_stop();
    esp_timer_delete(timer);

    ON9T_CHECK(arrived);
    ON9T_CHECK(g_ran_in_isr);
    ON9T_CHECK(packets.size() == 2);
#if ON9LOG_PLAIN_TEXT
    ON9T_CHECK(packets[1].text().find("[c-isr]") != std::string::npos);
    ON9T_CHECK(packets[1].text().find("isr 42") != std::string::npos);
#else
    const packet &isr = packets[1];
    ON9T_CHECK(isr.type() == ON9LOG_PKT_LOG && isr.level() == ON9_LOG_LEVEL_WARN);
    ON9T_CHECK(isr.seq() == static_cast<uint16_t>(packets[0].seq() + 1)); // numbered when written
    ON9T_CHECK(isr.time_ms() - packets[0].time_ms() < 100);
    ON9T_CHECK(isr.payload_len() == isr.payload.size());
    std::vector<uint8_t> expected = {1, ON9_LOG_ARGS_TYPE_32BITS};
    on9t::put32(expected, 42);
    ON9T_CHECK(isr.payload == expected);
#endif
}

void isr_drops_are_counted()
{
    ON9T_CHECK(on9log_esp_isr_init() == ESP_OK);

    // Strings cannot be read safely in an ISR; too many arguments do not fit a packet.
    ON9T_CHECK(!on9log_write_isr(ON9_LOG_LEVEL_INFO, "isr", "%s", "\x04", "text"));
    ON9T_CHECK(on9log_get_dropped_count() == 1);
#if !ON9LOG_PLAIN_TEXT
    // 18-byte header + 1 count + 13 types + 13 * 8 bytes = 136, over ON9LOG_ISR_PACKET_MAX (128).
    ON9T_CHECK(!on9log_write_isr(ON9_LOG_LEVEL_INFO, "isr", "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                                 "\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02\x02",
                                 1ull, 2ull, 3ull, 4ull, 5ull, 6ull, 7ull, 8ull, 9ull, 10ull, 11ull, 12ull, 13ull));
    ON9T_CHECK(on9log_get_dropped_count() == 2);
#endif
}

void isr_ringbuffer_overflow_is_reported()
{
    ON9T_CHECK(on9log_esp_isr_init() == ESP_OK);
    on9t::capture_start();

    // Holding the emit lock stops the drain task writing, so the ringbuffer fills up.
    on9log_port_emit_lock();
    size_t queued = 0;
    uint32_t refused = 0;
    for (int i = 0; i < 300; ++i) {
        if (on9log_write_isr(ON9_LOG_LEVEL_INFO, "isr", "fill %d", "\x01", i)) {
            ++queued;
        } else {
            ++refused;
        }
    }
    on9log_port_emit_unlock();

    // Every queued packet arrives, and DROPPED packets report every refused one. The
    // drain task may have taken its first pending count mid-loop, so there may be two.
    const bool arrived = wait_for_packets(queued + 1, 2000);
    vTaskDelay(pdMS_TO_TICKS(50));
    auto packets = on9t::capture_stop();
    ON9T_CHECK(arrived);
    ON9T_CHECK(refused > 0);
    size_t logs = 0;
    uint32_t reported = 0;
    for (const auto &p : packets) {
#if ON9LOG_PLAIN_TEXT
        // A drop line reads "...[on9log]: \033[0m<count> log message(s) dropped".
        const std::string text = p.text();
        const size_t drop_text = text.find(" log message(s) dropped");
        if (text.find("fill") != std::string::npos) {
            ++logs;
        } else if (drop_text != std::string::npos) {
            const size_t count_start = text.rfind("\033[0m", drop_text) + 4;
            reported += std::strtoul(text.c_str() + count_start, nullptr, 10);
        }
#else
        if (p.type() == ON9LOG_PKT_LOG) {
            ++logs;
        } else if (p.type() == ON9LOG_PKT_DROPPED) {
            reported += on9t::le32(p.payload, 0);
        }
#endif
    }
    ON9T_CHECK(logs == queued);
    ON9T_CHECK(reported == refused);
    ON9T_CHECK(on9log_get_dropped_count() == 0);
}

// ---------------------------------------------------------------------------
// The stdio VFS transport framing, written to a capture device
// ---------------------------------------------------------------------------

std::vector<uint8_t> g_vfs_bytes;

ssize_t capture_vfs_write(void *, int, const void *data, size_t size)
{
    const auto *bytes = static_cast<const uint8_t *>(data);
    g_vfs_bytes.insert(g_vfs_bytes.end(), bytes, bytes + size);
    return static_cast<ssize_t>(size);
}

int capture_vfs_open(void *, const char *, int, int)
{
    return 0;
}

int capture_vfs_close(void *, int)
{
    return 0;
}

void vfs_frames()
{
    static bool registered = false;
    if (!registered) {
        static esp_vfs_t vfs = {}; // unused operations stay NULL
        vfs.flags = ESP_VFS_FLAG_CONTEXT_PTR;
        vfs.write_p = capture_vfs_write;
        vfs.open_p = capture_vfs_open;
        vfs.close_p = capture_vfs_close;
        ON9T_CHECK(esp_vfs_register("/dev/on9t", &vfs, nullptr) == ESP_OK);
        ON9T_CHECK(esp_stdio_log_vfs_add_output("/dev/on9t/capture") == ESP_OK);
        registered = true;
    }
    g_vfs_bytes.clear();

    // The frame's own markers and the console's newline bytes must be escaped.
    const uint8_t special[] = {0xa5, 0xc0, 0xdb, 0x0a, 0x0d, 0x9a, 0x00, 0xff};
    ON9T_CHECK(esp_stdio_log_vfs_write_frame(ESP_STDIO_LOG_VFS_FRAME_TYPE_ON9LOG, special, sizeof(special)) == ESP_OK);
    ON9T_CHECK(esp_stdio_log_vfs_write_frame(ESP_STDIO_LOG_VFS_FRAME_TYPE_TEXT, nullptr, 0) == ESP_OK);
    std::vector<uint8_t> largest(ESP_STDIO_LOG_VFS_FRAME_MAX_PAYLOAD, 0x0a);
    ON9T_CHECK(esp_stdio_log_vfs_write_frame(ESP_STDIO_LOG_VFS_FRAME_TYPE_ON9LOG, largest.data(), largest.size()) == ESP_OK);
    largest.push_back(0);
    ON9T_CHECK(esp_stdio_log_vfs_write_frame(ESP_STDIO_LOG_VFS_FRAME_TYPE_ON9LOG, largest.data(), largest.size()) ==
               ESP_ERR_INVALID_SIZE);
    ON9T_CHECK(esp_stdio_log_vfs_write_frame(ESP_STDIO_LOG_VFS_FRAME_TYPE_TEXT, nullptr, 1) == ESP_ERR_INVALID_ARG);

    const auto frames = on9t::decode_frames(g_vfs_bytes);
    ON9T_CHECK(frames.size() == 3);
    ON9T_CHECK(frames[0].valid && frames[0].type == ESP_STDIO_LOG_VFS_FRAME_TYPE_ON9LOG);
    ON9T_CHECK(frames[0].payload == std::vector<uint8_t>(special, special + sizeof(special)));
    ON9T_CHECK(frames[1].valid && frames[1].type == ESP_STDIO_LOG_VFS_FRAME_TYPE_TEXT && frames[1].payload.empty());
    ON9T_CHECK(frames[2].valid && frames[2].payload.size() == ESP_STDIO_LOG_VFS_FRAME_MAX_PAYLOAD);
}

const on9t::test_case esp_cases[] = {
    {"addr_in_image", addr_in_image},
    {"image_id_is_elf_sha256", image_id_is_elf_sha256},
    {"reset_reason", reset_reason},
    {"timestamps_use_esp_timer", timestamps_use_esp_timer},
#if !ON9LOG_PLAIN_TEXT
    {"formats_live_in_noload", formats_live_in_noload},
#endif
    {"wire_order_on_two_cores", wire_order_on_two_cores},
    {"remove_sink_waits_for_packet_in_flight", remove_sink_waits_for_packet_in_flight},
    // These install the ISR backend, so they run after the core cases.
    {"isr_logs_reach_sinks", isr_logs_reach_sinks},
    {"isr_drops_are_counted", isr_drops_are_counted},
    {"isr_ringbuffer_overflow_is_reported", isr_ringbuffer_overflow_is_reported},
    {"vfs_frames", vfs_frames},
};

} // namespace

extern "C" void app_main(void)
{
    std::printf("ON9T START (%s%s)\n", ON9LOG_PLAIN_TEXT ? "plain text" : "binary",
                ON9LOG_CXX_REFLECTION ? ", C++26 reflection" : "");
    if (on9log_init() != ON9LOG_OK) {
        std::printf("ON9T RESULT: FAIL (on9log_init failed)\n");
    } else {
        on9t::run("core", on9t::core_cases, on9t::core_case_count);
        on9t::run("esp32", esp_cases, sizeof(esp_cases) / sizeof(esp_cases[0]));
        on9t::print_summary();
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // let the console drain
    esp_restart();
}
