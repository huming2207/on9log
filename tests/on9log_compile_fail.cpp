/*
 * Code that must NOT compile. Each ctest entry defines one ON9T_* macro and
 * passes only if the compiler stops with the expected message (see
 * on9log_add_compile_fail_test in CMakeLists.txt).
 */

#include "on9log.hpp"

#if defined(ON9T_TOO_MANY_ARGS)
void f(const Logger &log)
{
    log.info("33", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
             18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33);
}
#elif defined(ON9T_ISR_STRING_ARG)
void f(const Logger &log)
{
    log.isr_info("%s", "strings cannot be read in an ISR");
}
#elif defined(ON9T_UNSUPPORTED_TYPE)
struct not_loggable {
    int value;
};
void f(const Logger &log)
{
    log.info("{}", not_loggable{1});
}
#elif defined(ON9T_CONFIG_ONLY)
/* The #error under test comes from on9log_config.h, reached through the include above. */
#else
#error "define one ON9T_* case"
#endif
