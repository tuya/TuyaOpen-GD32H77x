/**
 * @file vw553_log.h
 * @brief Optional verbose diagnostics for the VW553 platform stack.
 */
#ifndef VW553_LOG_H
#define VW553_LOG_H

#include <stdio.h>

#if defined(__has_include)
#if __has_include("tuya_kconfig.h")
#include "tuya_kconfig.h"
#endif
#endif

/* Low-rate state transitions and timings stay visible even when per-frame AT
 * diagnostics are disabled. Keep this macro limited to a few lines per boot or
 * connection; high-volume transaction/traffic logs belong to VW553_LOG. */
#define VW553_SUMMARY(format, ...) printf("[VW553] " format "\r\n", ##__VA_ARGS__)

#if defined(GD32H77X_VW553_VERBOSE_LOG) && (GD32H77X_VW553_VERBOSE_LOG)
#define VW553_LOG(format, ...) printf("[VW553] " format "\r\n", ##__VA_ARGS__)
#else
#define VW553_LOG(format, ...) do { } while (0)
#endif

#endif
