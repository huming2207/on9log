#include "on9log.hpp"
#include "on9log_fmt.h"
#include "on9log_transport.h"
#include "on9log_unix_stdio.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

static void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

int main()
{
    FILE *capture = std::tmpfile();
    require(capture != nullptr);
    require(on9log_unix_stdio_init_file(capture) == ON9LOG_OK);

    Logger log("unix-smoke");
    log.info("value=%d", 42);
    log.info("name={} value={}", "host", 42);

    require(std::fflush(capture) == 0);
    require(std::fseek(capture, 0, SEEK_END) == 0);
    const long size = std::ftell(capture);
    unsigned char output[512]{};
    require(size > 0 && static_cast<unsigned long>(size) < sizeof(output));
    std::rewind(capture);

    const std::size_t length = std::fread(output, 1, sizeof(output), capture);
    require(length == static_cast<std::size_t>(size));
    output[length] = '\0';

#if ON9LOG_PLAIN_TEXT
    const char *text = reinterpret_cast<const char *>(output);
    require(std::strstr(text, "[unix-smoke]") != nullptr);
    require(std::strstr(text, "value=42") != nullptr);
    require(std::strstr(text, "name=host value=42") != nullptr);
    require(output[length - 1] == '\n');
#else
    std::size_t frame_offset = 0;
    static constexpr char metadata_prefix[] = "@on9log-image-slide=";
    if (length >= sizeof(metadata_prefix) - 1 &&
        std::memcmp(output, metadata_prefix, sizeof(metadata_prefix) - 1) == 0) {
        while (frame_offset < length && output[frame_offset] != '\n') {
            ++frame_offset;
        }
        require(frame_offset < length);
        ++frame_offset;
    }
    require(output[frame_offset] == 0xa5u);
    require(output[frame_offset + 1] == 0x01u);
    require(output[frame_offset + 2] == ON9LOG_PACKET_MAGIC);
    require(output[length - 1] == 0xc0u);

    require(std::fseek(capture, 0, SEEK_END) == 0);
    const long size_before_oversized = std::ftell(capture);
    unsigned char oversized[ON9LOG_TRANSPORT_MAX_PAYLOAD + 1u]{};
    oversized[0] = ON9LOG_PACKET_MAGIC;
    require(on9log_dispatch_packet(oversized, sizeof(oversized)) == ON9LOG_OK);
    require(std::fflush(capture) == 0);
    require(std::fseek(capture, 0, SEEK_END) == 0);
    require(std::ftell(capture) == size_before_oversized);
#endif

    require(std::fclose(capture) == 0);
    return 0;
}
