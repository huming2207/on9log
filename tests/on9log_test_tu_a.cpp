/*
 * One of two translation units whose first .noload format is the same, with
 * one format of their own after it. noload_formats_in_two_tus checks that
 * linking them keeps every format (see noload_format_storage in on9log.hpp).
 */

#include "on9log_test.h"

#include "on9log.hpp"

void on9t::tu_a_log()
{
    Logger log("tu-a");
    log.info<"on9t format shared by two TUs">();
    log.info<"on9t format only in TU a">();
}
