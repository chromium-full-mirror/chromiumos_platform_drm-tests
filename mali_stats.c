/*
 * Copyright 2022 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logging.h"
#include "mali/mali_gpu_perf_metrics.h"
#include "mali/mali_gpu_props.h"

const static int kDefaultSleepMicroseconds = 1000000;

void print_general_usage_percentage(int sleep_length) {
  int product_id = get_gpu_prop(gpu_prop_product_id);
  MaliGpuModel model = get_model_from_product_id(product_id);
  int max_freq_khz = get_gpu_prop(gpu_prop_freq_max_khz);

  MaliGpuCounter active_cycles = -1;
  for (int i = 0; i < kCounterBlockSize; i++) {
    if (mali_gpu_counter_names[model][gpu_counter_job_manager][i] &&
        strstr(mali_gpu_counter_names[model][gpu_counter_job_manager][i],
               "gpu_active")) {
      active_cycles = (model * kCounterBlockSize * kNumCounterBlockTypes) |
                      (gpu_counter_job_manager * kCounterBlockSize) | i;
      break;
    }
  }
  if (active_cycles == -1) {
    LOG_FATAL("No GPU active counter for model %d", model);
  }

  initialize_mali_perf_reader();
  reset_perf_metrics();
  usleep(sleep_length);
  struct mali_counter_response response = read_perf_metrics(&active_cycles, 1);
  cleanup_mali_perf_reader();

  printf("%f%% load\n", 100.0 * ((double)response.counter_values[0].values[0]) /
                            ((double)max_freq_khz) / 1000.0 /
                            ((double)sleep_length / 1000000.0));

  free_counters(response);
}

void print_all_available_counters(int sleep_length) {
  int product_id = get_gpu_prop(gpu_prop_product_id);
  MaliGpuModel model = get_model_from_product_id(product_id);

  int num_counters = 0;
  for (int i = 0; i < kNumCounterBlockTypes; i++) {
    for (int j = 0; j < kCounterBlockSize; j++) {
      if (mali_gpu_counter_names[model][i][j])
        num_counters++;
    }
  }

  MaliGpuCounter counters[num_counters];
  int counter_idx = 0;
  for (int i = 0; i < kNumCounterBlockTypes; i++) {
    for (int j = 0; j < kCounterBlockSize; j++) {
      if (mali_gpu_counter_names[model][i][j]) {
        counters[counter_idx] =
            (model * kCounterBlockSize * kNumCounterBlockTypes) |
            (i * kCounterBlockSize) | j;
        counter_idx++;
      }
    }
  }

  initialize_mali_perf_reader();
  reset_perf_metrics();
  usleep(sleep_length);
  struct mali_counter_response response =
      read_perf_metrics(counters, num_counters);
  cleanup_mali_perf_reader();

  for (int i = 0; i < response.num_counters; i++) {
    for (int j = 0; j < response.counter_values[i].num_values; j++) {
      int counter_type =
          (response.counter_values[i].counter / kCounterBlockSize) &
          (kNumCounterBlockTypes - 1);
      int counter_idx =
          response.counter_values[i].counter & (kCounterBlockSize - 1);
      printf("%s %d: %llu\n",
             mali_gpu_counter_names[model][counter_type][counter_idx], j,
             (unsigned long long)response.counter_values[i].values[j]);
    }
  }

  free_counters(response);
}

void print_help() {
  printf("mali_stats\n");
  printf("A simple program for querying Mali's performance counters.\n");
  printf("By default, this program will simply output %% GPU usage.\n");
  printf("Usage: mali_stats [-a] [-u sleep_length_in_microseconds]\n");
  printf("-u: Set the delay between clearing the counter and reading it.\n");
  printf("-a: Dump raw values for every available counter instead of %% ");
  printf("usage.\n");
}

int main(int argc, char** argv) {
  int sleep_length = kDefaultSleepMicroseconds;
  int c;
  bool print_all = false;

  while ((c = getopt(argc, argv, "ahu:")) != -1) {
    switch (c) {
      case 'u':
        sleep_length = atoi(optarg);
        break;
      case 'a':
        print_all = true;
        break;
      case 'h':
      default:
        print_help();
        exit(-1);
    }
  }

  if (print_all) {
    print_all_available_counters(sleep_length);
  } else {
    print_general_usage_percentage(sleep_length);
  }
}
