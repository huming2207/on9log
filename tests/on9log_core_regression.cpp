#include "on9log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

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

    return 0;
}
