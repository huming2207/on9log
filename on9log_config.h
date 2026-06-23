#pragma once

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#ifndef ON9_LOG_LOCAL_LEVEL
#ifdef CONFIG_ON9LOG_MAXIMUM_LEVEL
#define ON9_LOG_LOCAL_LEVEL CONFIG_ON9LOG_MAXIMUM_LEVEL
#else
#define ON9_LOG_LOCAL_LEVEL 3
#endif
#endif

#ifndef ON9LOG_MAX_SINKS
#ifdef CONFIG_ON9LOG_MAX_SINKS
#define ON9LOG_MAX_SINKS CONFIG_ON9LOG_MAX_SINKS
#else
#define ON9LOG_MAX_SINKS 4u
#endif
#endif
