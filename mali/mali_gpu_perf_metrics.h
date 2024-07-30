/*
 * Copyright 2022 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stddef.h>
#include <stdint.h>

#include "mali/mali_counters.h"

#ifndef __MALI_GPU_PERF_METRICS_H__
#define __MALI_GPU_PERF_METRICS_H__

typedef enum {
  gpu_counter_job_manager = 0,
  gpu_counter_tiler = 1,
  gpu_counter_shader = 2,
  gpu_counter_l2 = 3
} MaliGpuCounterType;

typedef enum {
  gpu_model_t60x = 0,
  gpu_model_t62x = 1,
  gpu_model_t72x = 2,
  gpu_model_t76x = 3,
  gpu_model_t82x = 4,
  gpu_model_t83x = 5,
  gpu_model_t86x = 6,
  gpu_model_tfrx = 7,
  gpu_model_tmix = 8,
  gpu_model_thex = 9,
  gpu_model_tsix = 10,
  gpu_model_tnox = 11,
  gpu_model_tgox = 12,
  gpu_model_tdvx = 13,
  gpu_model_ttrx = 14,
  gpu_model_tnax = 15,
  gpu_model_generic_csf = 16
} MaliGpuModel;

struct mali_counter_values {
  MaliGpuCounter counter;
  uint64_t* values;
  size_t num_values;
};

struct mali_counter_response {
  struct mali_counter_values* counter_values;
  size_t num_counters;
};

MaliGpuModel get_model_from_product_id(int product_id);
// TODO (greenjustin): Add a performance metrics context variable to keep state
// rather than relying on global variables.
void initialize_mali_perf_reader();
void free_counters(struct mali_counter_response counters);

extern void (*reset_perf_metrics)(void);
extern struct mali_counter_response (*read_perf_metrics)(MaliGpuCounter*,
                                                         size_t);
extern void (*cleanup_mali_perf_reader)(void);

#endif  // __MALI_GPU_PERF_METRICS_H__
