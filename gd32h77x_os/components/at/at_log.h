/**
 * @file log.h
 * @brief Optional verbose diagnostics for the AT platform stack.
 */
#ifndef AT_LOG_H
#define AT_LOG_H

#include <stdio.h>

/* Low-rate state transitions and timings stay visible even when per-frame AT
 * diagnostics are disabled. Keep this macro limited to a few lines per boot or
 * connection; high-volume transaction/traffic logs belong to AT_LOG. */
#define AT_SUMMARY(format, ...) printf("[AT] " format "\r\n", ##__VA_ARGS__)

/* Optional source-level debug switch for local AT investigations. */
#ifndef AT_VERBOSE_LOG
#define AT_VERBOSE_LOG 0
#endif

#if AT_VERBOSE_LOG
#define AT_LOG(format, ...) printf("[AT] " format "\r\n", ##__VA_ARGS__)
#else
#define AT_LOG(format, ...) do { } while (0)
#endif

#endif
