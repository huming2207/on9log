/**
 * @file on9log_unix_port.c
 * @brief Linux/macOS platform hooks for the on9log core.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "on9log_port.h"

#include <pthread.h>
#include <sched.h>
#include <time.h>

#if defined(__linux__)
#include <link.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#endif

static pthread_mutex_t s_on9log_lock = PTHREAD_MUTEX_INITIALIZER;

void on9log_port_lock(void)
{
    (void)pthread_mutex_lock(&s_on9log_lock);
}

void on9log_port_unlock(void)
{
    (void)pthread_mutex_unlock(&s_on9log_lock);
}

void on9log_port_yield(void)
{
    (void)sched_yield();
}

uint32_t on9log_port_timestamp_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }

    uint64_t milliseconds = (uint64_t)now.tv_sec * 1000u;
    milliseconds += (uint64_t)now.tv_nsec / 1000000u;
    return (uint32_t)milliseconds;
}

on9log_reset_reason_t on9log_port_reset_reason(void)
{
    return ON9LOG_RESET_UNKNOWN;
}

#if defined(__linux__) || defined(__APPLE__)
/* Read-only address ranges of the main executable, found once on first use. */
#define ON9LOG_UNIX_MAX_IMAGE_RANGES 8u

typedef struct {
    uintptr_t start;
    uintptr_t end;
} on9log_unix_range_t;

static on9log_unix_range_t s_image_ranges[ON9LOG_UNIX_MAX_IMAGE_RANGES];
static size_t s_image_range_count;
static pthread_once_t s_image_ranges_once = PTHREAD_ONCE_INIT;

#if defined(__linux__)
static int on9log_unix_collect_ranges(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    (void)data;

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
        if (phdr->p_type == PT_LOAD && (phdr->p_flags & PF_W) == 0 &&
            s_image_range_count < ON9LOG_UNIX_MAX_IMAGE_RANGES) {
            uintptr_t start = (uintptr_t)(info->dlpi_addr + phdr->p_vaddr);
            s_image_ranges[s_image_range_count++] = (on9log_unix_range_t){start, start + phdr->p_memsz};
        }
    }
    /* The first object reported is the main executable; the host only reads its ELF. */
    return 1;
}

static void on9log_unix_find_image_ranges(void)
{
    (void)dl_iterate_phdr(on9log_unix_collect_ranges, NULL);
}
#else
static void on9log_unix_find_image_ranges(void)
{
    /* String literals and const arrays live in __TEXT (__cstring, __const). */
    const struct mach_header_64 *header = (const struct mach_header_64 *)_dyld_get_image_header(0);
    unsigned long size = 0;
    uint8_t *text = header != NULL ? getsegmentdata(header, "__TEXT", &size) : NULL;

    if (text != NULL && size != 0) {
        s_image_ranges[0] = (on9log_unix_range_t){(uintptr_t)text, (uintptr_t)text + size};
        s_image_range_count = 1;
    }
}
#endif

bool on9log_port_addr_in_image(const void *addr)
{
    uintptr_t a = (uintptr_t)addr;

#if defined(__linux__)
    /* The wire ID is 32 bits, so a PIE address cannot be resolved by the host. */
    if (a > UINT32_MAX) {
        return false;
    }
#endif
    /* macOS IDs are truncated on purpose; the host corrects them with the image slide. */
    (void)pthread_once(&s_image_ranges_once, on9log_unix_find_image_ranges);
    for (size_t i = 0; i < s_image_range_count; ++i) {
        if (a >= s_image_ranges[i].start && a < s_image_ranges[i].end) {
            return true;
        }
    }
    return false;
}
#else
bool on9log_port_addr_in_image(const void *addr)
{
    (void)addr;
    return true;
}
#endif

uint32_t on9log_port_isr_timestamp_ms(void)
{
    return on9log_port_timestamp_ms();
}

bool on9log_port_isr_ready(void)
{
    return false;
}

bool on9log_port_isr_enqueue_packet(const uint8_t *packet, size_t len)
{
    (void)packet;
    (void)len;
    return false;
}
