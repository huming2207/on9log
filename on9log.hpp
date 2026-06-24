#pragma once

#ifndef __cplusplus
#error "on9log.hpp requires C++"
#endif

#if __cplusplus < 202002L
#error "on9log.hpp requires C++20 or newer; build this project as C++23"
#endif

#include "on9log.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifndef ON9FMT
#define ON9FMT(str) ON9_LOG_ATTR_STR(str)
#endif

namespace on9log {
namespace detail {

template <std::size_t N>
struct fixed_string {
    char value[N]{};

    constexpr fixed_string(const char (&str)[N]) noexcept
    {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }

    static constexpr std::size_t size = N;
};

template <fixed_string Format>
inline constexpr auto noload_format_storage ON9_LOG_NOLOAD_ATTR __attribute__((used)) = Format;

template <fixed_string Format>
constexpr const char *noload_format() noexcept
{
    return noload_format_storage<Format>.value;
}

template <fixed_string Format>
struct format_token {
};

template <typename>
inline constexpr bool dependent_false_v = false;

template <typename T>
using no_ref_t = std::remove_reference_t<T>;

template <typename T>
using raw_t = std::remove_cv_t<no_ref_t<T>>;

template <typename T>
struct is_char_pointer : std::false_type {};

template <typename T>
struct is_char_pointer<T *> : std::bool_constant<std::is_same_v<std::remove_cv_t<T>, char>> {};

template <typename T>
inline constexpr bool is_char_pointer_v = is_char_pointer<raw_t<T>>::value;

template <typename T>
struct is_char_array : std::false_type {};

template <std::size_t N>
struct is_char_array<char[N]> : std::true_type {};

template <typename T>
inline constexpr bool is_char_array_v = is_char_array<raw_t<T>>::value;

template <typename T>
inline constexpr bool is_supported_pointer_v =
    std::is_pointer_v<raw_t<T>> &&
    (std::is_void_v<std::remove_cv_t<std::remove_pointer_t<raw_t<T>>>> ||
     std::is_object_v<std::remove_pointer_t<raw_t<T>>>);

template <typename T>
consteval char arg_type_code()
{
    using Raw = raw_t<T>;

    if constexpr (is_char_pointer_v<T> || is_char_array_v<T>) {
        return static_cast<char>(ON9_LOG_ARGS_TYPE_DYNAMIC_STRING);
    } else if constexpr (std::is_array_v<Raw>) {
        return static_cast<char>(ON9_LOG_ARGS_TYPE_POINTER);
    } else if constexpr (std::is_null_pointer_v<Raw>) {
        return static_cast<char>(ON9_LOG_ARGS_TYPE_POINTER);
    } else if constexpr (std::is_pointer_v<Raw>) {
        static_assert(is_supported_pointer_v<T>,
                      "on9log C++ wrapper supports object pointers only; cast explicitly if needed");
        return static_cast<char>(ON9_LOG_ARGS_TYPE_POINTER);
    } else if constexpr (std::is_floating_point_v<Raw>) {
        static_assert(sizeof(Raw) <= sizeof(double),
                      "on9log C++ wrapper supports float and double, not wider floating-point types");
        return static_cast<char>(ON9_LOG_ARGS_TYPE_64BITS);
    } else if constexpr (std::is_integral_v<Raw> || std::is_enum_v<Raw>) {
        if constexpr (sizeof(Raw) > sizeof(std::uint32_t)) {
            static_assert(sizeof(Raw) <= sizeof(std::uint64_t),
                          "on9log C++ wrapper supports integer arguments up to 64 bits");
            return static_cast<char>(ON9_LOG_ARGS_TYPE_64BITS);
        } else {
            return static_cast<char>(ON9_LOG_ARGS_TYPE_32BITS);
        }
    } else {
        static_assert(dependent_false_v<T>,
                      "unsupported on9log C++ argument type; pass a scalar, pointer, or C string");
    }
}

template <typename... Args>
constexpr const char *arg_types() noexcept
{
    static_assert(sizeof...(Args) <= 255u, "on9log supports at most 255 encoded arguments");
    static constexpr char types[] = {arg_type_code<Args>()..., static_cast<char>(ON9_LOG_ARGS_TYPE_NONE)};
    return types;
}

template <typename T>
constexpr std::uint64_t float_bits(T value) noexcept
{
    if constexpr (std::is_same_v<raw_t<T>, float>) {
        return std::bit_cast<std::uint64_t>(static_cast<double>(value));
    } else {
        return std::bit_cast<std::uint64_t>(static_cast<double>(value));
    }
}

template <typename T>
constexpr auto integer_bits(T value) noexcept
{
    using Raw = raw_t<T>;
    if constexpr (std::is_enum_v<Raw>) {
        using Underlying = std::underlying_type_t<Raw>;
        if constexpr (sizeof(Underlying) > sizeof(std::uint32_t)) {
            return static_cast<std::uint64_t>(static_cast<Underlying>(value));
        } else {
            return static_cast<std::uint32_t>(static_cast<Underlying>(value));
        }
    } else if constexpr (sizeof(Raw) > sizeof(std::uint32_t)) {
        return static_cast<std::uint64_t>(value);
    } else {
        return static_cast<std::uint32_t>(value);
    }
}

template <typename T>
constexpr auto wire_arg(T &&value) noexcept
{
    using Raw = raw_t<T>;

    if constexpr (is_char_pointer_v<T>) {
        return static_cast<const char *>(value);
    } else if constexpr (is_char_array_v<T>) {
        return static_cast<const char *>(value);
    } else if constexpr (std::is_array_v<Raw>) {
        return static_cast<const void *>(value);
    } else if constexpr (std::is_null_pointer_v<Raw>) {
        return static_cast<const void *>(nullptr);
    } else if constexpr (std::is_pointer_v<Raw>) {
        static_assert(is_supported_pointer_v<T>,
                      "on9log C++ wrapper supports object pointers only; cast explicitly if needed");
        return static_cast<const void *>(value);
    } else if constexpr (std::is_floating_point_v<Raw>) {
        return float_bits(value);
    } else if constexpr (std::is_integral_v<Raw> || std::is_enum_v<Raw>) {
        return integer_bits(value);
    } else {
        static_assert(dependent_false_v<T>,
                      "unsupported on9log C++ argument type; pass a scalar, pointer, or C string");
    }
}

constexpr bool level_enabled(on9log_level_t level) noexcept
{
    return level != ON9_LOG_LEVEL_NONE && level <= ON9_LOG_LOCAL_LEVEL;
}

template <on9log_level_t Level, typename... Args>
void write_log(const char *tag, const char *format, Args &&...args) noexcept
{
    if constexpr (level_enabled(Level)) {
        on9log_write(Level,
                     tag,
                     format,
                     arg_types<Args...>(),
                     wire_arg(static_cast<Args &&>(args))...);
    }
}

template <on9log_level_t Level, fixed_string Format, typename... Args>
void write_log(const char *tag, format_token<Format>, Args &&...args) noexcept
{
    write_log<Level>(tag, noload_format<Format>(), static_cast<Args &&>(args)...);
}

template <on9log_level_t Level, typename... Args>
bool write_log_isr(const char *tag, const char *format, Args &&...args) noexcept
{
    if constexpr (!level_enabled(Level)) {
        return true;
    } else {
        static_assert(((arg_type_code<Args>() != static_cast<char>(ON9_LOG_ARGS_TYPE_DYNAMIC_STRING)) && ...),
                      "on9log ISR logging does not support dynamic string arguments");
        return on9log_write_isr(Level,
                                tag,
                                format,
                                arg_types<Args...>(),
                                wire_arg(static_cast<Args &&>(args))...);
    }
}

template <on9log_level_t Level, fixed_string Format, typename... Args>
bool write_log_isr(const char *tag, format_token<Format>, Args &&...args) noexcept
{
    return write_log_isr<Level>(tag, noload_format<Format>(), static_cast<Args &&>(args)...);
}

template <on9log_level_t Level>
void write_buffer(const char *tag, const void *buffer, std::size_t len) noexcept
{
    if constexpr (level_enabled(Level)) {
        on9log_write_buffer(Level, tag, buffer, len);
    }
}

} // namespace detail

namespace literals {

template <detail::fixed_string Format>
consteval detail::format_token<Format> operator""_on9fmt() noexcept
{
    return {};
}

} // namespace literals

class Logger {
public:
    constexpr explicit Logger(const char *tag) noexcept
        : tag_(tag)
    {
    }

    constexpr const char *tag() const noexcept { return tag_; }
    constexpr void set_tag(const char *tag) noexcept { tag_ = tag; }

    static void set_level(on9log_level_t level) noexcept
    {
        on9log_set_level(level);
    }

    static on9log_level_t level() noexcept
    {
        return on9log_get_level();
    }

    static on9log_err_t set_tag_level(const char *tag, on9log_level_t level) noexcept
    {
        return on9log_set_tag_level(tag, level);
    }

    static on9log_err_t clear_tag_level(const char *tag) noexcept
    {
        return on9log_clear_tag_level(tag);
    }

    template <typename... Args>
    void error(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_ERROR>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    void error(Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_ERROR>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    void error(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_ERROR>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <typename... Args>
    void warn(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_WARN>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    void warn(Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_WARN>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    void warn(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_WARN>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <typename... Args>
    void info(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_INFO>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    void info(Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_INFO>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    void info(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_INFO>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <typename... Args>
    void debug(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_DEBUG>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    void debug(Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_DEBUG>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    void debug(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_DEBUG>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <typename... Args>
    void verbose(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_VERBOSE>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    void verbose(Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_VERBOSE>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    void verbose(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_VERBOSE>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <on9log_level_t Level, typename... Args>
    void log(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<Level>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <on9log_level_t Level, detail::fixed_string Format, typename... Args>
    void log(Args &&...args) const noexcept
    {
        detail::write_log<Level>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <on9log_level_t Level, detail::fixed_string Format, typename... Args>
    void log(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<Level>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <typename... Args>
    bool isr_error(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_ERROR>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    bool isr_error(Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_ERROR>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    bool isr_error(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_ERROR>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <typename... Args>
    bool isr_warn(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_WARN>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    bool isr_warn(Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_WARN>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    bool isr_warn(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_WARN>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <typename... Args>
    bool isr_info(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_INFO>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    bool isr_info(Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_INFO>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    bool isr_info(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_INFO>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <typename... Args>
    bool isr_debug(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_DEBUG>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    bool isr_debug(Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_DEBUG>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    bool isr_debug(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_DEBUG>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <typename... Args>
    bool isr_verbose(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_VERBOSE>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    bool isr_verbose(Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_VERBOSE>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <detail::fixed_string Format, typename... Args>
    bool isr_verbose(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_VERBOSE>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <on9log_level_t Level, typename... Args>
    bool isr_log(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<Level>(tag_, format, static_cast<Args &&>(args)...);
    }

    template <on9log_level_t Level, detail::fixed_string Format, typename... Args>
    bool isr_log(Args &&...args) const noexcept
    {
        return detail::write_log_isr<Level>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    template <on9log_level_t Level, detail::fixed_string Format, typename... Args>
    bool isr_log(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<Level>(tag_, format, static_cast<Args &&>(args)...);
    }

    void buffer_error(const void *buffer, std::size_t len) const noexcept
    {
        detail::write_buffer<ON9_LOG_LEVEL_ERROR>(tag_, buffer, len);
    }

    void buffer_warn(const void *buffer, std::size_t len) const noexcept
    {
        detail::write_buffer<ON9_LOG_LEVEL_WARN>(tag_, buffer, len);
    }

    void buffer_info(const void *buffer, std::size_t len) const noexcept
    {
        detail::write_buffer<ON9_LOG_LEVEL_INFO>(tag_, buffer, len);
    }

    void buffer_debug(const void *buffer, std::size_t len) const noexcept
    {
        detail::write_buffer<ON9_LOG_LEVEL_DEBUG>(tag_, buffer, len);
    }

    void buffer_verbose(const void *buffer, std::size_t len) const noexcept
    {
        detail::write_buffer<ON9_LOG_LEVEL_VERBOSE>(tag_, buffer, len);
    }

    template <typename T, std::size_t N>
    void buffer_info(const T (&buffer)[N]) const noexcept
    {
        buffer_info(static_cast<const void *>(buffer), sizeof(buffer));
    }

    template <typename T, std::size_t N>
    void buffer_debug(const T (&buffer)[N]) const noexcept
    {
        buffer_debug(static_cast<const void *>(buffer), sizeof(buffer));
    }

private:
    const char *tag_;
};

inline constexpr Logger debug{"default"};

} // namespace on9log
