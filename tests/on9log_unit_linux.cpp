/*
 * Linux unit test: the shared core cases plus the Linux port's own behaviour
 * (pthread emit lock under real threads, ELF build-id parsing, address ranges).
 */

#include "on9log_test.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "on9log_port.h"

namespace {

// Packets must reach a sink in sequence order even when threads race. The sink
// yields to give other threads every chance to get between numbering and output.
std::mutex g_order_mutex;
std::vector<uint16_t> g_order;

void order_start(const uint8_t *header, size_t, void *)
{
    std::this_thread::yield();
    std::lock_guard<std::mutex> lock(g_order_mutex);
    g_order.push_back(static_cast<uint16_t>(header[2] | (header[3] << 8)));
}
void ignore_payload(const uint8_t *, size_t, size_t, size_t, void *) {}
void ignore_end(void *) {}
const on9log_sink_t order_sink = {order_start, ignore_payload, ignore_end};

void wire_order_matches_seq()
{
    if (ON9LOG_PLAIN_TEXT) {
        ON9T_SKIP("binary mode only: text lines have no sequence number");
    }
    constexpr int threads = 4;
    constexpr int logs_per_thread = 2000;
    g_order.clear();
    ON9T_CHECK(on9log_add_sink(&order_sink, nullptr) == ON9LOG_OK);
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([t] {
            for (int i = 0; i < logs_per_thread; ++i) {
                ON9_LOGI("order", "thread %d log %d", t, i);
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }
    ON9T_CHECK(on9log_remove_sink(&order_sink, nullptr) == ON9LOG_OK);

    ON9T_CHECK(g_order.size() == static_cast<size_t>(threads * logs_per_thread));
    for (size_t i = 1; i < g_order.size(); ++i) {
        ON9T_CHECK(g_order[i] == static_cast<uint16_t>(g_order[i - 1] + 1u));
    }
}

// A sink that can hold a packet open until the test releases it.
struct blocking_sink_state {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
};

void blocking_start(const uint8_t *, size_t, void *ctx)
{
    auto &state = *static_cast<blocking_sink_state *>(ctx);
    std::unique_lock lock(state.mutex);
    state.entered = true;
    state.cv.notify_all();
    state.cv.wait(lock, [&] { return state.release; });
}
const on9log_sink_t blocking_sink = {blocking_start, ignore_payload, ignore_end};

// on9log_remove_sink() waits for a packet in flight, so the caller may free the sink afterwards.
void remove_sink_waits_for_packet_in_flight()
{
    blocking_sink_state state;
    ON9T_CHECK(on9log_add_sink(&blocking_sink, &state) == ON9LOG_OK);

    std::thread logger([] { ON9_LOGI("block", "held open"); });
    {
        std::unique_lock lock(state.mutex);
        state.cv.wait(lock, [&] { return state.entered; });
    }

    std::atomic<bool> removed{false};
    std::thread remover([&] {
        (void)on9log_remove_sink(&blocking_sink, &state);
        removed.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool removed_early = removed.load();

    {
        std::lock_guard lock(state.mutex);
        state.release = true;
    }
    state.cv.notify_all();
    logger.join();
    remover.join();
    ON9T_CHECK(!removed_early);
    ON9T_CHECK(removed.load());
}

// Tag filters can be set and cleared while other threads look them up.
void tag_filters_under_concurrent_lookups()
{
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop.load()) {
            ON9_LOGI("reuse-a", "reader a");
            ON9_LOGI("reuse-b", "reader b");
        }
    });
    bool ok = true;
    for (unsigned i = 0; i < 500u; ++i) {
        char transient[] = "reuse-a";
        if ((i & 1u) != 0u) {
            transient[6] = 'b';
        }
        ok = ok && on9log_set_tag_level(transient, ON9_LOG_LEVEL_ERROR) == ON9LOG_OK;
        std::memset(transient, 'x', sizeof(transient) - 1u);
        ok = ok && on9log_clear_tag_level((i & 1u) != 0u ? "reuse-b" : "reuse-a") == ON9LOG_OK;
    }
    stop.store(true);
    reader.join();
    ON9T_CHECK(ok);
}

// Macro arguments with side effects run exactly once, however the type is detected.
void arguments_are_evaluated_once()
{
    int counter = 1;
    ON9_LOGI("once", "counter=%d", counter++);
    ON9_LOG_RUNTIMEI("once", "counter=%d", counter++);
    ON9T_CHECK(counter == 3);
}

// Appends one ELF note with the given alignment.
void put_note(std::vector<uint8_t> &notes, uint32_t type, const char *name, const std::vector<uint8_t> &desc, size_t align)
{
    const uint32_t fields[3] = {static_cast<uint32_t>(std::strlen(name) + 1), static_cast<uint32_t>(desc.size()), type};
    const auto *raw = reinterpret_cast<const uint8_t *>(fields);
    notes.insert(notes.end(), raw, raw + sizeof(fields));
    notes.insert(notes.end(), name, name + fields[0]);
    notes.resize((notes.size() + align - 1) / align * align, 0);
    notes.insert(notes.end(), desc.begin(), desc.end());
    notes.resize((notes.size() + align - 1) / align * align, 0);
}

void gnu_build_id_parser()
{
    const std::vector<uint8_t> build_id = {0x90, 0x05, 0x96, 0x59, 0xaa, 0xc0, 0x43, 0xe6, 0x00, 0x8a,
                                           0xca, 0xd7, 0x48, 0x94, 0xcd, 0x4b, 0x63, 0x65, 0x02, 0x05};
    uint8_t id[ON9LOG_IMAGE_ID_MAX_LEN];

    for (size_t align : {size_t{4}, size_t{8}}) {
        // The build-id follows another note, as in a real PT_NOTE segment.
        std::vector<uint8_t> notes;
        put_note(notes, 1, "GNU", {0, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0}, align);
        put_note(notes, 3, "GNU", build_id, align);
        ON9T_CHECK(on9log_find_gnu_build_id(notes.data(), notes.size(), align, id, sizeof(id)) == build_id.size());
        ON9T_CHECK(std::memcmp(id, build_id.data(), build_id.size()) == 0);

        // Too small a destination, a cut-off note, or no build-id note: nothing found.
        ON9T_CHECK(on9log_find_gnu_build_id(notes.data(), notes.size(), align, id, build_id.size() - 1) == 0);
        ON9T_CHECK(on9log_find_gnu_build_id(notes.data(), notes.size() - 8, align, id, sizeof(id)) == 0);
        std::vector<uint8_t> other;
        put_note(other, 3, "XYZ", build_id, align);
        ON9T_CHECK(on9log_find_gnu_build_id(other.data(), other.size(), align, id, sizeof(id)) == 0);
    }

    // A corrupt size field must not read past the buffer; NULL input finds nothing.
    std::vector<uint8_t> corrupt;
    put_note(corrupt, 3, "GNU", build_id, 4);
    corrupt[4] = 0xff;
    corrupt[5] = 0xff;
    ON9T_CHECK(on9log_find_gnu_build_id(corrupt.data(), corrupt.size(), 4, id, sizeof(id)) == 0);
    ON9T_CHECK(on9log_find_gnu_build_id(nullptr, 64, 4, id, sizeof(id)) == 0);
}

void linux_port()
{
    // The test links without PIE: literals are in a read-only segment below 4 GiB.
    static int writable = 0;
    char stack[] = "stack";
    ON9T_CHECK(on9log_port_addr_in_image("literal"));
    ON9T_CHECK(!on9log_port_addr_in_image(&writable));
    ON9T_CHECK(!on9log_port_addr_in_image(stack));
    std::vector<char> heap(8, 'h');
    ON9T_CHECK(!on9log_port_addr_in_image(heap.data()));

    uint8_t id[ON9LOG_IMAGE_ID_MAX_LEN];
    on9log_image_id_kind_t kind = ON9LOG_IMAGE_ID_NONE;
    ON9T_CHECK(on9log_port_image_id(id, sizeof(id), &kind) > 0); // the test links with --build-id
    ON9T_CHECK(kind == ON9LOG_IMAGE_ID_GNU_BUILD_ID);
    ON9T_CHECK(on9log_port_reset_reason() == ON9LOG_RESET_UNKNOWN);

    const uint32_t before = on9log_port_timestamp_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint32_t elapsed = on9log_port_timestamp_ms() - before;
    ON9T_CHECK(elapsed >= 20 && elapsed < 2000);
}

const on9t::test_case linux_cases[] = {
    {"wire_order_matches_seq", wire_order_matches_seq},
    {"remove_sink_waits_for_packet_in_flight", remove_sink_waits_for_packet_in_flight},
    {"tag_filters_under_concurrent_lookups", tag_filters_under_concurrent_lookups},
    {"arguments_are_evaluated_once", arguments_are_evaluated_once},
    {"gnu_build_id_parser", gnu_build_id_parser},
    {"linux_port", linux_port},
};

} // namespace

int main()
{
    if (on9log_init() != ON9LOG_OK) {
        return 1;
    }
    on9t::run("core", on9t::core_cases, on9t::core_case_count);
    on9t::run("linux", linux_cases, sizeof(linux_cases) / sizeof(linux_cases[0]));
    return on9t::print_summary() == 0 ? 0 : 1;
}
