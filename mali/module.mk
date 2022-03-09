# Copyright 2022 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include common.mk

CFLAGS += -std=gnu99 -I$(SRC)

CC_STATIC_LIBRARY(libmali.pic.a): \
 mali/mali_counters.o \
 mali/mali_gpu_props.o \
 mali/mali_gpu_perf_metrics.o
