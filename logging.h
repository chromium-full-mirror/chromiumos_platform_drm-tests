/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdio.h>
#include <stdlib.h>

#ifndef __LOGGING_H__
#define __LOGGING_H__

enum logging_levels {
  kLoggingDebug = -1,
  kLoggingInfo = 0,
  kLoggingError,
  kLoggingFatal,
  kLoggingLevelMax
};

#define DEFAULT_LOG_LEVEL kLoggingInfo

#define LOG(level, stream, fmt, ...)       \
  do {                                     \
    if (level >= DEFAULT_LOG_LEVEL) {      \
      fprintf(stream, fmt, ##__VA_ARGS__); \
      fprintf(stream, "\n");               \
      fflush(stream);                      \
    }                                      \
  } while (0)

#define LOG_DEBUG(fmt, ...) LOG(kLoggingDebug, stderr, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG(kLoggingInfo, stderr, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG(kLoggingError, stderr, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...)                         \
  do {                                              \
    LOG(kLoggingFatal, stderr, fmt, ##__VA_ARGS__); \
    exit(EXIT_FAILURE);                             \
  } while (0)

#endif  // __LOGGING_H__
