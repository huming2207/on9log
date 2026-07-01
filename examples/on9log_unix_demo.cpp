#define ON9_LOG_LOCAL_LEVEL 5

#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>

#include <sys/resource.h>
#include <sys/utsname.h>

#include "on9log.h"
#include "on9log.hpp"
#include "on9log_unix_stdio.h"

static const char *TAG = "demo";

typedef struct {
    uint32_t start_count;
    uint32_t end_count;
    uint32_t payload_count;
    uint32_t payload_bytes;
} demo_sink_stats_t;

static demo_sink_stats_t s_demo_sink_stats;
static const auto s_start_time = std::chrono::steady_clock::now();

static uint32_t host_tick_ms()
{
    const auto elapsed = std::chrono::steady_clock::now() - s_start_time;
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

static uint64_t host_max_rss_bytes()
{
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024u;
#endif
}

static void demo_sink_start(const uint8_t *header, size_t header_len, void *ctx)
{
    demo_sink_stats_t *stats = static_cast<demo_sink_stats_t *>(ctx);

    (void)header;
    (void)header_len;
    ++stats->start_count;
}

static void demo_sink_payload(const uint8_t *payload,
                              size_t payload_len,
                              size_t total_arg_cnt,
                              size_t curr_arg_index,
                              void *ctx)
{
    demo_sink_stats_t *stats = static_cast<demo_sink_stats_t *>(ctx);

    (void)payload;
    (void)total_arg_cnt;
    (void)curr_arg_index;
    ++stats->payload_count;
    stats->payload_bytes += static_cast<uint32_t>(payload_len);
}

static void demo_sink_end(void *ctx)
{
    demo_sink_stats_t *stats = static_cast<demo_sink_stats_t *>(ctx);
    ++stats->end_count;
}

static const on9log_sink_t s_demo_sink = {
    .start_cb = demo_sink_start,
    .payload_cb = demo_sink_payload,
    .end_cb = demo_sink_end,
};

static void fill_pattern(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(i * 37u + 11u);
    }
}

static void log_host_info()
{
    struct utsname host {};
    const bool have_uname = uname(&host) == 0;
    const char *target = have_uname ? host.sysname : "unix";
    const char *machine = have_uname ? host.machine : "unknown";
    const unsigned cores = std::thread::hardware_concurrency();

    std::printf("plain text: hello from printf after on9log stdio init\n");
    std::fprintf(stderr, "plain text stderr: target=%s\n", target);
    std::fprintf(stdout, "host log plain-text line through stdio\n");

    ON9_LOGI(TAG,
             "host=%s machine=%s cores=%u features pthread=1 stdio=1",
             target,
             machine,
             cores);
    ON9_LOGI(TAG,
             "max_rss=%" PRIu64 " bytes uptime=%" PRIu32 " ms",
             host_max_rss_bytes(),
             host_tick_ms());
}

static void log_format_cases()
{
    const char *name = "on9log";
    const char *null_string = nullptr;
    const uint8_t pointer_target[] = {0xa5, 0xc0, 0xdb, 0x00};
    const uint8_t *pointer_arg = pointer_target;
    int signed_value = -42;
    unsigned unsigned_value = 0xfeedbeefu;
    long long signed_64 = -0x112233445566ll;
    unsigned long long unsigned_64 = 0x1122334455667788ull;
    int width = 10;
    int negative_width = -8;
    int precision = 5;
    const char *right = "right";
    const char *left = "left";
    const char *truncate = "truncate-me";

    ON9_LOGE(TAG, "level error signed=%d", signed_value);
    ON9_LOGW(TAG, "level warn unsigned=%u hex=%08x", unsigned_value, unsigned_value);
    ON9_LOGI(TAG, "level info string=%s null=%s", name, null_string);
    ON9_LOGD(TAG,
             "level debug ptr=%p bytes=%02x:%02x",
             pointer_arg,
             pointer_target[0],
             pointer_target[1]);
    ON9_LOGV(TAG, "level verbose ll=%lld ull=%llu", signed_64, unsigned_64);

    ON9_LOGI(TAG, "lengths hhd=%hhd hhu=%hhu hd=%hd hu=%hu", -1, 255, -1234, 54321);
    ON9_LOGI(TAG, "widths [%*s] [%-*s] [%.*s]", width, right, negative_width, left, precision, truncate);
    ON9_LOGI(TAG, "percent literal 100%% and char=%c", 'Z');
    ON9_LOGI(TAG, "empty log with no printf args");
}

static void log_string_slice_cases()
{
    on9log::Logger log("demo");

    const char c_text[] = "slice!";
    const char non_nul_text[] = {'s', 'l', 'i', 'c', 'e', '!'};
    const int short_len = 5;
    const int full_len = static_cast<int>(std::strlen(c_text));
    const std::string owned = "owned-string-demo";
    const std::string_view view(non_nul_text, sizeof(non_nul_text));
    const std::string_view partial_view(non_nul_text + 1, 4);

    ON9_LOGI(TAG,
             "---- string slice demo: C %%.*s, C++ std::string_view, C++ std::string ----");
    ON9_LOGI(TAG,
             "C precision slice short len=%d value=[%.*s]",
             short_len,
             short_len,
             c_text);
    ON9_LOGI(TAG,
             "C precision slice full len=%d value=[%.*s]",
             full_len,
             full_len,
             c_text);

    log.info(ON9FMT("C++ string_view full len={} value=[{}]"), static_cast<unsigned>(view.size()), view);
    log.info(ON9FMT("C++ string_view partial len={} value=[{}]"),
             static_cast<unsigned>(partial_view.size()),
             partial_view);
    log.info(ON9FMT("C++ std::string len={} value=[{}]"), static_cast<unsigned>(owned.size()), owned);
}

static void log_cpp_format_cases()
{
    on9log::Logger log("demo");
    const char *name = "on9log";
    const uint8_t pointer_target[] = {0xa5, 0xc0, 0xdb, 0x00};
    const uint8_t *pointer_arg = pointer_target;
    unsigned u32_value = 42u;
    unsigned hex_value = 0xabadu;
    int signed_value = -42;
    unsigned long long u64_value = 0x1122334455667788ull;
    long long signed_64 = -42ll;
    double pi = 3.141592653589793;
    double neg_pi = -3.5;
    int width = 10;
    int precision = 3;
    const char *text = "foobar";

    log.info("cpp defaults int={} ptr={} str={}", u32_value, pointer_arg, name);
    log.info("cpp signed and unsigned 64-bit values: {} {}", signed_64, u64_value);
    log.info("cpp float default={} fixed={:.6f}", pi, pi);
    log.info("cpp type chars d={:d} x={:x} X={:X} o={:o} b={:b} B={:B} p={} s={:s} c={:c}",
             u32_value, hex_value, hex_value, u32_value, u32_value, u32_value,
             pointer_arg, name, 'Z');
    log.info("cpp alt form {:#x} {:#o} {:#b}", hex_value, u32_value, u32_value);

    log.info("cpp align [{:>10}] [{:<10}] [{:^10}] [{:*>10}] [{:*<10}]",
             u32_value, name, u32_value, u32_value, name);
    log.info("cpp pad_and_sign {:08x} {:#08x} [{:>8}] {:+d} {: d}",
             hex_value, hex_value, u32_value, signed_value, signed_value);

    log.info("cpp string_precision {:.3s} {:.0s} {:.100s}", text, text, text);
    log.info("cpp integer_width {:04d} {:04x} {:#06x} {:+04d}",
             u32_value, hex_value, hex_value, signed_value);

    log.info("cpp floats {:.2f} {:.2F} {:e} {:.2e} default {}", pi, pi, pi, pi, pi);
    log.info("cpp floats_neg_space {:.1f} {:+.1f} {: .1f} {:+.1f}", neg_pi, pi, pi, neg_pi);
    log.info("cpp inf_nan {} {}", static_cast<double>(INFINITY), static_cast<double>(NAN));

    log.info("cpp dyn_width_auto [{:{}}] [{:<{}}]", u32_value, width, name, width);
    log.info("cpp dyn_precision_auto {:.{}f} {:.{}s}", pi, precision, text, precision);
    log.info("cpp dyn_width_precision_auto [{:{}.{}f}] [{:>{}}]",
             pi, width, precision, name, width);

    log.info("cpp positional {0}-{1}-{0}", u32_value, hex_value);
    log.info("cpp explicit_w {0:{1}}", u32_value, width);
    log.info("cpp explicit_p {0:.{1}f}", pi, precision);
    log.info("cpp explicit_reuse {0:{0}}", width);
    log.info("cpp brace_escapes {{}} and {}", u32_value);

    unsigned complex_id = 0xabu;
    log.info("cpp complex [{:>8.2f}] {:s}={:#06x}", pi, name, complex_id);
}

static void log_logger_wrapper_cases()
{
    using namespace on9log::literals;

    on9log::Logger log("demo");

    const char *name = "on9log";
    unsigned value = 42u;
    unsigned hex_value = 0xabadu;
    unsigned long long u64_value = 0x1122334455667788ull;
    long long signed_64 = -42ll;
    double pi = 3.141592653589793;
    const uint8_t pointer_target[] = {0xa5, 0xc0, 0xdb, 0x00};
    const uint8_t *pointer_arg = pointer_target;

    ON9_LOGI(TAG,
             "---- on9log::Logger wrapper demo: 4 call forms, same message ----");

    log.info("wrapper plain literal value={} name={}", value, name);
    log.info(ON9FMT("wrapper ON9FMT form value={} name={}"), value, name);
    log.info<"wrapper NTTP form value={} name={}">(value, name);
    log.info("wrapper UDL form value={} name={}"_on9fmt, value, name);

    ON9_LOGI(TAG, "---- wrapper: each level using fmt syntax ----");
    log.error(ON9FMT("wrapper level error value={}"), value);
    log.warn(ON9FMT("wrapper level warn name={}"), name);
    log.info(ON9FMT("wrapper level info ptr={}"), pointer_arg);
    log.debug(ON9FMT("wrapper level debug value={} hex={:x}"), value, hex_value);
    log.verbose(ON9FMT("wrapper level verbose u64={} signed_64={}"), u64_value, signed_64);

    ON9_LOGI(TAG, "---- wrapper: generic log<Level,...> template form ----");
    log.log<ON9_LOG_LEVEL_INFO, "wrapper log<INFO,...> form value={}">(value);
    log.log<ON9_LOG_LEVEL_WARN, "wrapper log<WARN,...> form value={}">(value);

    ON9_LOGI(TAG, "---- wrapper: printf-style on the C++ wrapper ----");
    log.warn("wrapper printf style value=%u hex=%08x", value, hex_value);
    log.info("wrapper printf strings name=%s completion=100%%", name);

    ON9_LOGI(TAG, "---- wrapper: fmt-style remains separate from printf-style ----");
    log.info("wrapper fmt float fixed={:.2f} default={}", pi, pi);
    log.info("wrapper fmt u64={} signed_64={}", u64_value, signed_64);

    ON9_LOGI(TAG, "---- wrapper: buffer dump pointer+len form ----");
    log.buffer_info(pointer_target, sizeof(pointer_target));

    ON9_LOGI(TAG, "---- wrapper: buffer dump array convenience overload ----");
    uint8_t small_array[] = {0x10, 0x20, 0x30, 0x40};
    log.buffer_info(small_array);
}

static void log_buffer_cases()
{
    uint8_t small[] = {
        0x00, 0x01, 0x02, 0x03, 0x7f, 0x80, 0xa5, 0xc0,
        0xdb, 0xde, 0xdd, 0xdc, 0xf0, 0x55, 0xaa, 0xff,
        'O',  'N',  '9',  '\n',
    };
    static uint8_t large[3200];

    ON9_LOG_BUFI(TAG, small, sizeof(small));
    ON9_LOG_BUFI(TAG, nullptr, 0);

    fill_pattern(large, sizeof(large));
    ON9_LOGI(TAG, "next buffer exercises multi-packet host stdio output");
    ON9_LOG_BUFI(TAG, large, sizeof(large));
    ON9_LOGI(TAG, "this line follows the large host buffer dump");

    on9log_write_buffer(ON9_LOG_LEVEL_INFO, TAG, nullptr, 1);
    ON9_LOGI(TAG, "invalid NULL buffer produced dropped_count=%" PRIu32, on9log_get_dropped_count());
}

static void log_sink_cases()
{
    std::memset(&s_demo_sink_stats, 0, sizeof(s_demo_sink_stats));
    const char *buffer_text = "sink-buffer";
    on9log_err_t add_err = on9log_add_sink(&s_demo_sink, &s_demo_sink_stats);
    ON9_LOGI(TAG, "secondary sink add err=%d", static_cast<int>(add_err));

    ON9_LOGI(TAG, "secondary sink sees this formatted packet value=%d", 1234);
    ON9_LOG_BUFI(TAG, buffer_text, std::strlen(buffer_text));

    on9log_err_t remove_err = on9log_remove_sink(&s_demo_sink, &s_demo_sink_stats);
    ON9_LOGI(
        TAG,
        "secondary sink remove err=%d starts=%" PRIu32 " ends=%" PRIu32 " payload_cb=%" PRIu32 " payload_bytes=%" PRIu32,
        static_cast<int>(remove_err),
        s_demo_sink_stats.start_count,
        s_demo_sink_stats.end_count,
        s_demo_sink_stats.payload_count,
        s_demo_sink_stats.payload_bytes);
}

static void emit_filter_demo_lines(const char *tag, const char *label)
{
    ON9_LOGE(tag, "[%s] error", label);
    ON9_LOGW(tag, "[%s] warn", label);
    ON9_LOGI(tag, "[%s] info", label);
    ON9_LOGD(tag, "[%s] debug", label);
    ON9_LOGV(tag, "[%s] verbose", label);
}

static void log_filter_cases()
{
    ON9_LOGI(TAG,
             "---- filter baseline: default_runtime=%d compile_ceiling=%d ----",
             static_cast<int>(on9log_get_level()), ON9_LOG_LOCAL_LEVEL);
    emit_filter_demo_lines("demo", "baseline");
    emit_filter_demo_lines("worker", "baseline");

    ON9_LOGW(TAG, "---- filter: global runtime level -> WARN (info line below suppressed) ----");
    on9log_set_level(ON9_LOG_LEVEL_WARN);
    ON9_LOGI(TAG, "this info line should be suppressed");
    emit_filter_demo_lines("demo", "global=WARN");
    emit_filter_demo_lines("worker", "global=WARN");

    ON9_LOGW(TAG, "---- filter: per-tag worker -> VERBOSE via C++ static wrapper ----");
    on9log::Logger::set_tag_level("worker", ON9_LOG_LEVEL_VERBOSE);
    emit_filter_demo_lines("demo", "per-tag worker=VERBOSE");
    emit_filter_demo_lines("worker", "per-tag worker=VERBOSE");

    ON9_LOGW(TAG, "---- filter: clear worker override via C++ static wrapper ----");
    on9log::Logger::clear_tag_level("worker");
    emit_filter_demo_lines("demo", "cleared worker");
    emit_filter_demo_lines("worker", "cleared worker");

    ON9_LOGW(TAG,
             "---- filter: global NONE next (no log lines should appear until restore) ----");
    on9log_set_level(ON9_LOG_LEVEL_NONE);
    emit_filter_demo_lines("demo", "global=NONE");
    ON9_LOGE(TAG, "this error line must be suppressed under NONE");
    on9log::Logger::set_level(ON9_LOG_LEVEL_VERBOSE);
    ON9_LOGI(TAG, "---- filter: global VERBOSE restored (NONE block above showed nothing) ----");
    emit_filter_demo_lines("demo", "restored");
    emit_filter_demo_lines("worker", "restored");
}

static void worker_task(const char *task_name)
{
    for (int i = 0; i < 5; ++i) {
        std::printf("plain text worker %s iteration %d\n", task_name, i);
        ON9_LOGI("worker",
                 "task=%s iteration=%d tick=%" PRIu32,
                 task_name,
                 i,
                 host_tick_ms());
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    ON9_LOGW("worker", "task=%s finished", task_name);
}

int main()
{
    on9log_err_t on9_err = on9log_init();
    if (on9_err != ON9LOG_OK) {
        std::printf("on9log_init failed: %d\n", static_cast<int>(on9_err));
        return 1;
    }

    on9_err = on9log_unix_stdio_init();
    if (on9_err != ON9LOG_OK) {
        std::printf("on9log_unix_stdio_init failed: %d\n", static_cast<int>(on9_err));
        return 1;
    }

    ON9_LOGI(TAG,
             "on9log demo start local_level=%d plain_text=%d",
             ON9_LOG_LOCAL_LEVEL,
             ON9LOG_PLAIN_TEXT);
    log_host_info();
    log_format_cases();
    log_string_slice_cases();
    log_cpp_format_cases();
    log_logger_wrapper_cases();
    log_buffer_cases();
    log_sink_cases();
    log_filter_cases();

    std::thread worker(worker_task, "A");
    ON9_LOGI(TAG, "worker task create result=%d", 1);

    for (uint32_t heartbeat = 0; heartbeat < 6; ++heartbeat) {
        ON9_LOGI(TAG,
                 "main heartbeat=%" PRIu32 " max_rss=%" PRIu64,
                 heartbeat,
                 host_max_rss_bytes());
        std::printf("plain text main heartbeat %" PRIu32 "\n", heartbeat);
        std::fprintf(stdout, "host log plain text main heartbeat %" PRIu32 "\n", heartbeat);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    worker.join();
    ON9_LOGI(TAG, "on9log demo complete; final host heartbeat follows");
    ON9_LOGI(TAG, "slow heartbeat tick=%" PRIu32, host_tick_ms());
    return 0;
}
