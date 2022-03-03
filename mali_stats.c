/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mali/mali_gpu_perf_metrics.h"
#include "mali/mali_gpu_props.h"

int main(int argc, char** argv) {
  initialize_mali_perf_reader();

  MaliGpuCounter counters[] = {tnax_gpu_active, tnax_tiler_active,
                               tnax_compute_active, tnax_mmu_requests};
  struct mali_counter_response response;
  response = read_perf_metrics(counters, 4);

  assert(response.num_counters == 4);

  printf("Counter TNAx GPU_ACTIVE value(s):\n");
  for (int j = 0; j < response.counter_values[0].num_values; j++) {
    printf("%d\n", response.counter_values[0].values[j]);
  }

  printf("\n");
  printf("Counter TNAx TILER_ACTIVE value(s):\n");
  for (int j = 0; j < response.counter_values[1].num_values; j++) {
    printf("%d\n", response.counter_values[1].values[j]);
  }

  printf("\n");
  printf("Counter TNAx COMPUTE_ACTIVE value(s):\n");
  for (int j = 0; j < response.counter_values[2].num_values; j++) {
    printf("%d\n", response.counter_values[2].values[j]);
  }

  printf("\n");
  printf("Counter TNAx MMU_REQUESTS value(s):\n");
  for (int j = 0; j < response.counter_values[3].num_values; j++) {
    printf("%d\n", response.counter_values[3].values[j]);
  }

  free_counters(response);

  cleanup_mali_perf_reader();
}
