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

/**
 * @brief  Macro to annotate a format string literal with the appropriate
 *         section attribute for flash placement.
 *
 * When @c ON9FMT is not defined before inclusion it defaults to
 * @c ON9_LOG_ATTR_STR(str).  Users may define it before including this header
 * to override the attribute strategy.
 *
 * @param str  A string literal to annotate.
 */
#ifndef ON9FMT
#define ON9FMT(str) ON9_LOG_ATTR_STR(str)
#endif

/**
 * @brief  Top-level namespace for the on9log C++ wrapper.
 *
 * Contains the @ref Logger class, the @ref detail implementation namespace,
 * and the @ref literals namespace for user-defined literal operators.
 */
namespace on9log {

// -----------------------------------------------------------------------
// detail namespace  --  compile-time helpers and low-level senders
// -----------------------------------------------------------------------

/**
 * @brief  Implementation details for the on9log C++ wrapper.
 *
 * This namespace is not intended for direct use by client code.  It provides
 * the compile-time type-traits, argument encoding, and the actual
 * @c on9log_write / @c on9log_write_isr / @c on9log_write_buffer call sites
 * used by the public @ref Logger API.
 */
namespace detail {

/**
 * @brief  A @c fixed_string wrapper that stores a character array as a
 *         non-type template parameter (NTTP).
 *
 * C++20 allows class types as NTTPs; @c fixed_string lets callers pass a
 * string literal as a template argument, e.g.
 * @code
 *   Logger::info<"hello %d">(42);
 * @endcode
 * The stored string is then available at compile time and can be placed in a
 * no-load (noload) section by the linker.
 *
 * @tparam N  Size of the string array (including the null terminator).
 */
template <std::size_t N>
struct fixed_string {
    /// The character buffer, value-initialised to zero.
    char value[N]{};

    /** @brief  Construct from a string literal.
     *  @param str  Source string of length N (including null terminator).
     */
    constexpr fixed_string(const char (&str)[N]) noexcept
    {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }

    /// Compile-time size of the string (includes null terminator).
    static constexpr std::size_t size = N;
};

/**
 * @brief  Per-format instantiation of a @ref fixed_string placed in a
 *         no-load (noload) ELF section.
 *
 * The @c ON9_LOG_NOLOAD_ATTR and @c __attribute__((used)) ensure the storage
 * survives linker garbage collection and lands in the designated noload
 * section so the format string does not occupy RAM at runtime.
 *
 * @tparam Format  A @ref fixed_string NTTP containing the format literal.
 */
template <fixed_string Format>
inline constexpr auto noload_format_storage ON9_LOG_NOLOAD_ATTR __attribute__((used)) = Format;

/**
 * @brief  Return a pointer to the noload-stored format string.
 *
 * @tparam Format  A @ref fixed_string NTTP containing the format literal.
 * @return Pointer to the character buffer of the noload storage.
 */
template <fixed_string Format>
constexpr const char *noload_format() noexcept
{
    return noload_format_storage<Format>.value;
}

/**
 * @brief  Empty tag type that selects the NTTP / token-based logging path.
 *
 * When a function is overloaded on @c format_token<Format> the compiler
 * resolves to the overload that obtains the format string from
 * @ref noload_format instead of from a run-time pointer, keeping the format
 * in the noload section.
 *
 * @tparam Format  A @ref fixed_string NTTP identifying the format literal.
 */
template <fixed_string Format>
struct format_token {
};

/**
 * @brief  Helper that always evaluates to @c false, used in
 *         @c static_assert to produce a dependent-false trick.
 *
 * @tparam  Unused; any type satisfies the definition.
 */
template <typename>
inline constexpr bool dependent_false_v = false;

/** @brief  Remove reference qualification from @p T.
 *  @tparam T  The type to strip. */
template <typename T>
using no_ref_t = std::remove_reference_t<T>;

/** @brief  Remove cv-qualifiers and reference from @p T (canonical raw type).
 *  @tparam T  The type to strip. */
template <typename T>
using raw_t = std::remove_cv_t<no_ref_t<T>>;

/**
 * @brief  Trait: @c true when @p T is a pointer to (possibly cv-qualified)
 *         @c char.
 *
 * Primary template inherits @c std::false_type.
 * @tparam T  The type to test.
 */
template <typename T>
struct is_char_pointer : std::false_type {};

/**
 * @brief  Specialisation: @c T* is a char-pointer when @c T (cv-unqualified)
 *         is @c char.
 * @tparam T  The pointed-to type (cv-qualified @c char).
 */
template <typename T>
struct is_char_pointer<T *> : std::bool_constant<std::is_same_v<std::remove_cv_t<T>, char>> {};

/** @brief  Convenience variable template for @ref is_char_pointer.
 *  @tparam T  The type to test. */
template <typename T>
inline constexpr bool is_char_pointer_v = is_char_pointer<raw_t<T>>::value;

/**
 * @brief  Trait: @c true when @p T is a @c char[N] array.
 *
 * Primary template inherits @c std::false_type.
 * @tparam T  The type to test.
 */
template <typename T>
struct is_char_array : std::false_type {};

/**
 * @brief  Specialisation: @c char[N] is a char-array.
 * @tparam N  Array extent.
 */
template <std::size_t N>
struct is_char_array<char[N]> : std::true_type {};

/** @brief  Convenience variable template for @ref is_char_array.
 *  @tparam T  The type to test. */
template <typename T>
inline constexpr bool is_char_array_v = is_char_array<raw_t<T>>::value;

/**
 * @brief  Variable template that is @c true when @p T is a pointer to a
 *         supported object type (object or void).
 *
 * Function pointers and other non-object pointer types are excluded.
 * @tparam T  The type to test.
 */
template <typename T>
inline constexpr bool is_supported_pointer_v =
    std::is_pointer_v<raw_t<T>> &&
    (std::is_void_v<std::remove_cv_t<std::remove_pointer_t<raw_t<T>>>> ||
     std::is_object_v<std::remove_pointer_t<raw_t<T>>>);

/**
 * @brief  Map a C++ argument type to the corresponding
 *         @c ON9_LOG_ARGS_TYPE_* code at compile time.
 *
 * The mapping rules are:
 *  - char pointers/arrays -> @c ON9_LOG_ARGS_TYPE_DYNAMIC_STRING
 *  - other arrays         -> @c ON9_LOG_ARGS_TYPE_POINTER
 *  - null pointer         -> @c ON9_LOG_ARGS_TYPE_POINTER
 *  - object/void pointers -> @c ON9_LOG_ARGS_TYPE_POINTER
 *  - float / double       -> @c ON9_LOG_ARGS_TYPE_64BITS
 *  - integer / enum up to 32 bits -> @c ON9_LOG_ARGS_TYPE_32BITS
 *  - integer / enum larger than 32 bits -> @c ON9_LOG_ARGS_TYPE_64BITS
 *  - anything else triggers a @c static_assert failure.
 *
 * @tparam T  The argument type (as passed, may include cv/ref).
 * @return The single-character type code recognised by the C API.
 */
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

/**
 * @brief  Build the on9log argument-type descriptor string for a pack of
 *         arguments.
 *
 * The returned character array contains one @c ON9_LOG_ARGS_TYPE_* code per
 * argument, terminated by @c ON9_LOG_ARGS_TYPE_NONE.  The array is
 * statically allocated per unique set of argument types.
 *
 * @tparam Args  The argument types (as passed, may include cv/ref).
 * @return Pointer to a static @c constexpr @c char array describing the
 *         argument pack.
 */
template <typename... Args>
constexpr const char *arg_types() noexcept
{
    static_assert(sizeof...(Args) <= 255u, "on9log supports at most 255 encoded arguments");
    static constexpr char types[] = {arg_type_code<Args>()..., static_cast<char>(ON9_LOG_ARGS_TYPE_NONE)};
    return types;
}

/**
 * @brief  Convert a floating-point argument to the 64-bit wire representation
 *         (always @c double stored in a @c uint64_t via @c std::bit_cast).
 *
 * @tparam T  A floating-point type (@c float or @c double).
 * @param value  The value to convert.
 * @return The bit pattern of @c static_cast<double>(value) as a
 *         @c std::uint64_t.
 */
template <typename T>
constexpr std::uint64_t float_bits(T value) noexcept
{
    if constexpr (std::is_same_v<raw_t<T>, float>) {
        return std::bit_cast<std::uint64_t>(static_cast<double>(value));
    } else {
        return std::bit_cast<std::uint64_t>(static_cast<double>(value));
    }
}

/**
 * @brief  Convert an integer or enum argument to its wire representation.
 *
 * Enums are first cast to their underlying type.  Values wider than 32 bits
 * are promoted to @c std::uint64_t; narrower values use @c std::uint32_t.
 *
 * @tparam T  An integral or enumeration type.
 * @param value  The value to convert.
 * @return A @c std::uint32_t or @c std::uint64_t containing the value.
 */
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

/**
 * @brief  Convert an arbitrary argument to the wire type expected by the
 *         on9log C API.
 *
 * Dispatch rules:
 *  - char pointer / char array -> @c const char*
 *  - other array / pointer     -> @c const void*
 *  - floating point           -> @ref float_bits result
 *  - integer / enum          -> @ref integer_bits result
 *  - otherwise              -> @c static_assert failure
 *
 * @tparam T  The argument type (as forwarded, may include ref).
 * @param value  The argument value.
 * @return The value converted to the appropriate on9log wire type.
 */
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

/**
 * @brief  Check whether a given log level is enabled at compile time.
 *
 * A level is enabled when it is not @c ON9_LOG_LEVEL_NONE and its numeric
 * value is <= @c ON9_LOG_LOCAL_LEVEL.  This function is @c constexpr so
 * disabled levels can be elided entirely by the compiler.
 *
 * @param level  The log level to test.
 * @return @c true if the level is enabled, @c false otherwise.
 */
constexpr bool level_enabled(on9log_level_t level) noexcept
{
    return level != ON9_LOG_LEVEL_NONE && level <= ON9_LOG_LOCAL_LEVEL;
}

/**
 * @brief  Write a log message (plain format-string overload).
 *
 * If @p Level is enabled at compile time, calls
 * @c on9log_write with the auto-detected argument type codes and wire
 * values.  Otherwise the call is a no-op.
 *
 * @tparam Level  The on9log severity level.
 * @tparam Args   Argument types (inferred from the actual arguments).
 * @param tag     Tag string (typically the Logger's tag).
 * @param format  printf-style format string (in flash / .rodata).
 * @param args    The variadic arguments to encode.
 */
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

/**
 * @brief  Write a log message (token / NTTP format overload).
 *
 * Retrieves the format string from @ref noload_format (which resides in a
 * noload ELF section) and delegates to the plain-format overload.
 *
 * @tparam Level   The on9log severity level.
 * @tparam Format  A @ref fixed_string NTTP carrying the format literal.
 * @tparam Args    Argument types (inferred from the actual arguments).
 * @param tag      Tag string.
 * @param          (unused)  A @ref format_token tag for overload resolution.
 * @param args     The variadic arguments to encode.
 */
template <on9log_level_t Level, fixed_string Format, typename... Args>
void write_log(const char *tag, format_token<Format>, Args &&...args) noexcept
{
    write_log<Level>(tag, noload_format<Format>(), static_cast<Args &&>(args)...);
}

/**
 * @brief  Write a log message from an ISR context (plain format-string
 *         overload).
 *
 * Behaves like @ref write_log but calls @c on9log_write_isr instead.
 * Returns @c false if the ring buffer is full (and the message was dropped),
 * @c true on success.
 *
 * @note Dynamic string arguments (@c ON9_LOG_ARGS_TYPE_DYNAMIC_STRING) are
 *       rejected at compile time because ISR logging cannot safely access
 *       string data that may be in RAM subject to corruption.
 *
 * @tparam Level  The on9log severity level.
 * @tparam Args   Argument types (inferred from the actual arguments).
 * @param tag     Tag string.
 * @param format  printf-style format string.
 * @param args    The variadic arguments to encode.
 * @return @c true if the message was queued successfully, @c false if the
 *         ISR ring buffer was full.
 */
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

/**
 * @brief  Write a log message from an ISR context (token / NTTP format
 *         overload).
 *
 * Retrieves the format string from noload storage and delegates to the
 * plain-format ISR overload.
 *
 * @tparam Level   The on9log severity level.
 * @tparam Format  A @ref fixed_string NTTP carrying the format literal.
 * @tparam Args    Argument types (inferred from the actual arguments).
 * @param tag      Tag string.
 * @param          (unused)  A @ref format_token tag for overload resolution.
 * @param args     The variadic arguments to encode.
 * @return @c true if the message was queued successfully, @c false if the
 *         ISR ring buffer was full.
 */
template <on9log_level_t Level, fixed_string Format, typename... Args>
bool write_log_isr(const char *tag, format_token<Format>, Args &&...args) noexcept
{
    return write_log_isr<Level>(tag, noload_format<Format>(), static_cast<Args &&>(args)...);
}

/**
 * @brief  Write a raw memory buffer as a hex-dump log entry.
 *
 * Calls @c on9log_write_buffer when @p Level is enabled; otherwise a no-op.
 *
 * @tparam Level  The on9log severity level.
 * @param tag     Tag string.
 * @param buffer  Pointer to the buffer to dump.
 * @param len     Number of bytes in the buffer.
 */
template <on9log_level_t Level>
void write_buffer(const char *tag, const void *buffer, std::size_t len) noexcept
{
    if constexpr (level_enabled(Level)) {
        on9log_write_buffer(Level, tag, buffer, len);
    }
}

} // namespace detail

// -----------------------------------------------------------------------
// literals namespace  --  user-defined literal operators
// -----------------------------------------------------------------------

/**
 * @brief  User-defined string literal operators for on9log format strings.
 *
 * Include this namespace with a @c using directive to enable the
 * @c "_on9fmt" suffix:
 * @code
 *   using namespace on9log::literals;
 *   logger.info("count: %d"_on9fmt, 42);
 * @endcode
 */
namespace literals {

/**
 * @brief  User-defined literal that produces a @ref detail::format_token
 *         from a string literal at compile time.
 *
 * @code
 *   "hello %d"_on9fmt   // yields detail::format_token<"hello %d">
 * @endcode
 *
 * @tparam Format  The literal characters wrapped as a @ref detail::fixed_string.
 * @return A default-constructed @ref detail::format_token aiding overload
 *         resolution toward the NTTP/noload logging path.
 */
template <detail::fixed_string Format>
consteval detail::format_token<Format> operator""_on9fmt() noexcept
{
    return {};
}

} // namespace literals

// -----------------------------------------------------------------------
// Logger class  --  main public API
// -----------------------------------------------------------------------

/**
 * @brief  Lightweight, header-only C++20 logging front-end for the on9log C
 *         library.
 *
 * Each @c Logger instance holds an optional tag string and exposes
 * per-level logging methods in three flavours:
 *
 *  1. **Plain format** -- the format string is a plain @c const char*
 *     argument kept in flash via @c ON9FMT.
 *  2. **NTTP format** -- the format string is a non-type template parameter
 *     (e.g. @c logger.info<"count %d">(42)), which places it in a noload
 *     ELF section.
 *  3. **Token format** -- the format string is wrapped in a
 *     @c detail::format_token obtained via @c operator""_on9fmt, also
 *     routing through the noload section.
 *
 * The class is compatible with @c -fno-exceptions and @c -fno-rtti; all
 * public methods are @c noexcept.
 *
 * @note  All logging methods are @c const -- they do not modify the
 *        Logger itself.
 *
 * @warning  The tag string is stored as a raw pointer; the caller must
 *           ensure the pointed-to string outlives the Logger.
 */
class Logger {
public:
    /**
     * @brief  Construct a Logger with a given tag.
     *
     * @param tag  A C-string tag; must remain valid for the lifetime of
     *             this Logger.
     */
    constexpr explicit Logger(const char *tag) noexcept
        : tag_(tag)
    {
    }

    /** @brief  Return the current tag string.
     *  @return Pointer to the tag C-string. */
    constexpr const char *tag() const noexcept { return tag_; }

    /** @brief  Replace the tag string.
     *  @param tag  New C-string tag; must remain valid for the lifetime of
     *              this Logger. */
    constexpr void set_tag(const char *tag) noexcept { tag_ = tag; }

    /**
     * @brief  Set the global log level (affects all loggers).
     *
     * Delegates to @c on9log_set_level.
     *
     * @param level  The new global severity threshold.
     */
    static void set_level(on9log_level_t level) noexcept
    {
        on9log_set_level(level);
    }

    /**
     * @brief  Return the current global log level.
     *
     * @return The current @c on9log_level_t threshold.
     */
    static on9log_level_t level() noexcept
    {
        return on9log_get_level();
    }

    /**
     * @brief  Override the log level for a specific tag.
     *
     * Delegates to @c on9log_set_tag_level.
     *
     * @param tag    The tag to configure.
     * @param level  The per-tag severity threshold.
     * @return @c ON9_LOG_OK on success, or an error code from the C API.
     */
    static on9log_err_t set_tag_level(const char *tag, on9log_level_t level) noexcept
    {
        return on9log_set_tag_level(tag, level);
    }

    /**
     * @brief  Remove a per-tag level override, falling back to the global
     *         level.
     *
     * Delegates to @c on9log_clear_tag_level.
     *
     * @param tag  The tag whose override should be removed.
     * @return @c ON9_LOG_OK on success, or an error code from the C API.
     */
    static on9log_err_t clear_tag_level(const char *tag) noexcept
    {
        return on9log_clear_tag_level(tag);
    }

    // ------------------------------------------------------------------
    // error()  --  three overloads per level
    // ------------------------------------------------------------------

    /**
     * @brief  Log a message at ERROR level (plain format string).
     *
     * @tparam Args  Argument types (inferred).
     * @param format  printf-style format string (typically in flash via
     *                @c ON9FMT).
     * @param args    Variadic arguments.
     */
    template <typename... Args>
    void error(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_ERROR>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at ERROR level (NTTP format string).
     *
     * The format is passed as a non-type template parameter and placed in a
     * noload ELF section.
     *
     * @tparam Format  A @ref detail::fixed_string NTTP containing the
     *                 format literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     */
    template <detail::fixed_string Format, typename... Args>
    void error(Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_ERROR>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at ERROR level (token format string).
     *
     * @tparam Format  A @ref detail::fixed_string NTTP embedded in the
     *                 @ref detail::format_token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value (typically from
     *                 @c operator""_on9fmt).
     * @param args     Variadic arguments.
     */
    template <detail::fixed_string Format, typename... Args>
    void error(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_ERROR>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // warn()
    // ------------------------------------------------------------------

    /**
     * @brief  Log a message at WARN level (plain format string).
     *
     * @tparam Args  Argument types (inferred).
     * @param format  printf-style format string.
     * @param args    Variadic arguments.
     */
    template <typename... Args>
    void warn(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_WARN>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at WARN level (NTTP format string).
     *
     * @tparam Format  A @ref detail::fixed_string NTTP with the format
     *                 literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     */
    template <detail::fixed_string Format, typename... Args>
    void warn(Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_WARN>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at WARN level (token format string).
     *
     * @tparam Format  Format literal embedded in the token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value.
     * @param args     Variadic arguments.
     */
    template <detail::fixed_string Format, typename... Args>
    void warn(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_WARN>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // info()
    // ------------------------------------------------------------------

    /**
     * @brief  Log a message at INFO level (plain format string).
     *
     * @tparam Args  Argument types (inferred).
     * @param format  printf-style format string.
     * @param args    Variadic arguments.
     */
    template <typename... Args>
    void info(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_INFO>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at INFO level (NTTP format string).
     *
     * @tparam Format  A @ref detail::fixed_string NTTP with the format
     *                 literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     */
    template <detail::fixed_string Format, typename... Args>
    void info(Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_INFO>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at INFO level (token format string).
     *
     * @tparam Format  Format literal embedded in the token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value.
     * @param args     Variadic arguments.
     */
    template <detail::fixed_string Format, typename... Args>
    void info(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_INFO>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // debug()
    // ------------------------------------------------------------------

    /**
     * @brief  Log a message at DEBUG level (plain format string).
     *
     * @tparam Args  Argument types (inferred).
     * @param format  printf-style format string.
     * @param args    Variadic arguments.
     */
    template <typename... Args>
    void debug(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_DEBUG>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at DEBUG level (NTTP format string).
     *
     * @tparam Format  A @ref detail::fixed_string NTTP with the format
     *                 literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     */
    template <detail::fixed_string Format, typename... Args>
    void debug(Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_DEBUG>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at DEBUG level (token format string).
     *
     * @tparam Format  Format literal embedded in the token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value.
     * @param args     Variadic arguments.
     */
    template <detail::fixed_string Format, typename... Args>
    void debug(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_DEBUG>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // verbose()
    // ------------------------------------------------------------------

    /**
     * @brief  Log a message at VERBOSE level (plain format string).
     *
     * @tparam Args  Argument types (inferred).
     * @param format  printf-style format string.
     * @param args    Variadic arguments.
     */
    template <typename... Args>
    void verbose(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_VERBOSE>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at VERBOSE level (NTTP format string).
     *
     * @tparam Format  A @ref detail::fixed_string NTTP with the format
     *                 literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     */
    template <detail::fixed_string Format, typename... Args>
    void verbose(Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_VERBOSE>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at VERBOSE level (token format string).
     *
     * @tparam Format  Format literal embedded in the token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value.
     * @param args     Variadic arguments.
     */
    template <detail::fixed_string Format, typename... Args>
    void verbose(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<ON9_LOG_LEVEL_VERBOSE>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // log()  --  generic level as a template parameter
    // ------------------------------------------------------------------

    /**
     * @brief  Log a message at an arbitrary level (plain format string).
     *
     * @tparam Level  The on9log severity level (e.g. @c ON9_LOG_LEVEL_INFO).
     * @tparam Args   Argument types (inferred).
     * @param format  printf-style format string.
     * @param args    Variadic arguments.
     */
    template <on9log_level_t Level, typename... Args>
    void log(const char *format, Args &&...args) const noexcept
    {
        detail::write_log<Level>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at an arbitrary level (NTTP format string).
     *
     * @tparam Level   The on9log severity level.
     * @tparam Format  A @ref detail::fixed_string NTTP with the format
     *                 literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     */
    template <on9log_level_t Level, detail::fixed_string Format, typename... Args>
    void log(Args &&...args) const noexcept
    {
        detail::write_log<Level>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  Log a message at an arbitrary level (token format string).
     *
     * @tparam Level   The on9log severity level.
     * @tparam Format  Format literal embedded in the token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value.
     * @param args     Variadic arguments.
     */
    template <on9log_level_t Level, detail::fixed_string Format, typename... Args>
    void log(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        detail::write_log<Level>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // isr_error()  --  ISR-safe logging, returns bool
    // ------------------------------------------------------------------

    /**
     * @brief  ISR-safe log at ERROR level (plain format string).
     *
     * @tparam Args  Argument types (inferred); dynamic strings are rejected.
     * @param format  printf-style format string.
     * @param args    Variadic arguments.
     * @return @c true if the message was queued, @c false if the ISR buffer
     *         was full.
     */
    template <typename... Args>
    bool isr_error(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_ERROR>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at ERROR level (NTTP format string).
     *
     * @tparam Format  A @ref detail::fixed_string NTTP with the format
     *                 literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <detail::fixed_string Format, typename... Args>
    bool isr_error(Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_ERROR>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at ERROR level (token format string).
     *
     * @tparam Format  Format literal embedded in the token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value.
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <detail::fixed_string Format, typename... Args>
    bool isr_error(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_ERROR>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // isr_warn()
    // ------------------------------------------------------------------

    /**
     * @brief  ISR-safe log at WARN level (plain format string).
     *
     * @tparam Args  Argument types (inferred).
     * @param format  printf-style format string.
     * @param args    Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <typename... Args>
    bool isr_warn(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_WARN>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at WARN level (NTTP format string).
     *
     * @tparam Format  A @ref detail::fixed_string NTTP with the format
     *                 literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <detail::fixed_string Format, typename... Args>
    bool isr_warn(Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_WARN>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at WARN level (token format string).
     *
     * @tparam Format  Format literal embedded in the token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value.
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <detail::fixed_string Format, typename... Args>
    bool isr_warn(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_WARN>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // isr_info()
    // ------------------------------------------------------------------

    /**
     * @brief  ISR-safe log at INFO level (plain format string).
     *
     * @tparam Args  Argument types (inferred).
     * @param format  printf-style format string.
     * @param args    Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <typename... Args>
    bool isr_info(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_INFO>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at INFO level (NTTP format string).
     *
     * @tparam Format  A @ref detail::fixed_string NTTP with the format
     *                 literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <detail::fixed_string Format, typename... Args>
    bool isr_info(Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_INFO>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at INFO level (token format string).
     *
     * @tparam Format  Format literal embedded in the token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value.
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <detail::fixed_string Format, typename... Args>
    bool isr_info(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_INFO>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // isr_debug()
    // ------------------------------------------------------------------

    /**
     * @brief  ISR-safe log at DEBUG level (plain format string).
     *
     * @tparam Args  Argument types (inferred).
     * @param format  printf-style format string.
     * @param args    Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <typename... Args>
    bool isr_debug(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_DEBUG>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at DEBUG level (NTTP format string).
     *
     * @tparam Format  A @ref detail::fixed_string NTTP with the format
     *                 literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <detail::fixed_string Format, typename... Args>
    bool isr_debug(Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_DEBUG>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at DEBUG level (token format string).
     *
     * @tparam Format  Format literal embedded in the token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value.
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <detail::fixed_string Format, typename... Args>
    bool isr_debug(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_DEBUG>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // isr_verbose()
    // ------------------------------------------------------------------

    /**
     * @brief  ISR-safe log at VERBOSE level (plain format string).
     *
     * @tparam Args  Argument types (inferred).
     * @param format  printf-style format string.
     * @param args    Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <typename... Args>
    bool isr_verbose(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_VERBOSE>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at VERBOSE level (NTTP format string).
     *
     * @tparam Format  A @ref detail::fixed_string NTTP with the format
     *                 literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <detail::fixed_string Format, typename... Args>
    bool isr_verbose(Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_VERBOSE>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at VERBOSE level (token format string).
     *
     * @tparam Format  Format literal embedded in the token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value.
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <detail::fixed_string Format, typename... Args>
    bool isr_verbose(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<ON9_LOG_LEVEL_VERBOSE>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // isr_log()  --  generic ISR level as template parameter
    // ------------------------------------------------------------------

    /**
     * @brief  ISR-safe log at an arbitrary level (plain format string).
     *
     * @tparam Level  The on9log severity level.
     * @tparam Args   Argument types (inferred).
     * @param format  printf-style format string.
     * @param args    Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <on9log_level_t Level, typename... Args>
    bool isr_log(const char *format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<Level>(tag_, format, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at an arbitrary level (NTTP format string).
     *
     * @tparam Level   The on9log severity level.
     * @tparam Format  A @ref detail::fixed_string NTTP with the format
     *                 literal.
     * @tparam Args    Argument types (inferred).
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <on9log_level_t Level, detail::fixed_string Format, typename... Args>
    bool isr_log(Args &&...args) const noexcept
    {
        return detail::write_log_isr<Level>(tag_, detail::format_token<Format>{}, static_cast<Args &&>(args)...);
    }

    /**
     * @brief  ISR-safe log at an arbitrary level (token format string).
     *
     * @tparam Level   The on9log severity level.
     * @tparam Format  Format literal embedded in the token.
     * @tparam Args    Argument types (inferred).
     * @param format   A @ref detail::format_token value.
     * @param args     Variadic arguments.
     * @return @c true if queued, @c false if the ISR buffer was full.
     */
    template <on9log_level_t Level, detail::fixed_string Format, typename... Args>
    bool isr_log(detail::format_token<Format> format, Args &&...args) const noexcept
    {
        return detail::write_log_isr<Level>(tag_, format, static_cast<Args &&>(args)...);
    }

    // ------------------------------------------------------------------
    // buffer_*()  --  hex-dump log helpers
    // ------------------------------------------------------------------

    /**
     * @brief  Log a buffer hex dump at ERROR level.
     *
     * @param buffer  Pointer to the data to dump.
     * @param len     Number of bytes.
     */
    void buffer_error(const void *buffer, std::size_t len) const noexcept
    {
        detail::write_buffer<ON9_LOG_LEVEL_ERROR>(tag_, buffer, len);
    }

    /**
     * @brief  Log a buffer hex dump at WARN level.
     *
     * @param buffer  Pointer to the data to dump.
     * @param len     Number of bytes.
     */
    void buffer_warn(const void *buffer, std::size_t len) const noexcept
    {
        detail::write_buffer<ON9_LOG_LEVEL_WARN>(tag_, buffer, len);
    }

    /**
     * @brief  Log a buffer hex dump at INFO level.
     *
     * @param buffer  Pointer to the data to dump.
     * @param len     Number of bytes.
     */
    void buffer_info(const void *buffer, std::size_t len) const noexcept
    {
        detail::write_buffer<ON9_LOG_LEVEL_INFO>(tag_, buffer, len);
    }

    /**
     * @brief  Log a buffer hex dump at DEBUG level.
     *
     * @param buffer  Pointer to the data to dump.
     * @param len     Number of bytes.
     */
    void buffer_debug(const void *buffer, std::size_t len) const noexcept
    {
        detail::write_buffer<ON9_LOG_LEVEL_DEBUG>(tag_, buffer, len);
    }

    /**
     * @brief  Log a buffer hex dump at VERBOSE level.
     *
     * @param buffer  Pointer to the data to dump.
     * @param len     Number of bytes.
     */
    void buffer_verbose(const void *buffer, std::size_t len) const noexcept
    {
        detail::write_buffer<ON9_LOG_LEVEL_VERBOSE>(tag_, buffer, len);
    }

    /**
     * @brief  Log a typed array as a hex dump at INFO level.
     *
     * Convenience overload that takes a C-style array by reference and
     * automatically computes its size.
     *
     * @tparam T  Array element type.
     * @tparam N  Array extent (inferred).
     * @param buffer  The array to dump.
     */
    template <typename T, std::size_t N>
    void buffer_info(const T (&buffer)[N]) const noexcept
    {
        buffer_info(static_cast<const void *>(buffer), sizeof(buffer));
    }

    /**
     * @brief  Log a typed array as a hex dump at DEBUG level.
     *
     * Convenience overload that takes a C-style array by reference and
     * automatically computes its size.
     *
     * @tparam T  Array element type.
     * @tparam N  Array extent (inferred).
     * @param buffer  The array to dump.
     */
    template <typename T, std::size_t N>
    void buffer_debug(const T (&buffer)[N]) const noexcept
    {
        buffer_debug(static_cast<const void *>(buffer), sizeof(buffer));
    }

private:
    /// Owning reference to the tag C-string (not owned; must outlive this).
    const char *tag_;
};

/**
 * @brief  Pre-built Logger instance with tag @c "default" at file-scope.
 *
 * Logging through this instance does not require constructing a Logger
 * manually:
 * @code
 *   on9log::debug.info("hello");
 * @endcode
 */
inline constexpr Logger debug{"default"};

} // namespace on9log
