/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "mali/mali_gpu_perf_metrics.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "logging.h"
#include "mali/mali_gpu_props.h"
#include "mali/mali_ioctl.h"

int num_shader_cores;
int num_l2_caches;
MaliGpuModel curr_model;

int gpufd = -1;
int reader_fd = -1;

struct kbase_hwcnt_reader_metadata reader_metadata;

// The model logic here is also taken from gfx-pps.
MaliGpuModel get_model_from_product_id(int product_id) {
  MaliGpuModel model;
  int masked_product_id = product_id & 0xF00F;
  switch (product_id) {
    case 0x6956:
      model = gpu_model_t60x;
      break;
    case 0x0620:
      model = gpu_model_t62x;
      break;
    case 0x0720:
      model = gpu_model_t72x;
      break;
    case 0x0750:
      model = gpu_model_t76x;
      break;
    case 0x0820:
      model = gpu_model_t82x;
      break;
    case 0x0830:
      model = gpu_model_t83x;
      break;
    case 0x0860:
      model = gpu_model_t86x;
      break;
    case 0x0880:
      model = gpu_model_tfrx;
      break;
    default:
      switch (masked_product_id) {
        case 0x6000:
          model = gpu_model_tmix;
          break;
        case 0x6001:
          model = gpu_model_thex;
          break;
        case 0x7000:
          model = gpu_model_tsix;
          break;
        case 0x7001:
          model = gpu_model_tnox;
          break;
        case 0x7002:
          model = gpu_model_tgox;
          break;
        case 0x7003:
          model = gpu_model_tdvx;
          break;
        case 0x9000:
          model = gpu_model_ttrx;
          break;
        case 0x9001:
        case 0x9003:
          model = gpu_model_tnax;
          break;
        default:
          LOG_FATAL("Error: unsupported GPU with product ID 0x%x", product_id);
      }
      break;
  }

  return model;
}

void initialize_mali_perf_reader() {
  int product_id = get_gpu_prop(gpu_prop_product_id);
  curr_model = get_model_from_product_id(product_id);
  num_shader_cores =
      __builtin_popcount(get_gpu_prop(gpu_prop_shader_present_mask));
  num_l2_caches = get_gpu_prop(gpu_prop_num_l2);

  gpufd = open(kGpuDevice, O_RDWR | O_CLOEXEC);
  if (gpufd < 0)
    LOG_FATAL("Error opening GPU device! %s\n", strerror(errno));

  struct kbase_ioctl_version_check version_check;
  if (ioctl(gpufd, KBASE_IOCTL_VERSION_CHECK, &version_check) < 0)
    LOG_FATAL("Error checking GPU version! %s\n", strerror(errno));
  assert(version_check.major >= SUPPORTED_MAJOR_VERSION);
  assert(version_check.minor >= SUPPORTED_MINOR_VERSION);

  struct kbase_ioctl_set_flags init_flags;
  init_flags.create_flags = BASE_CONTEXT_SYSTEM_MONITOR_SUBMIT_DISABLED;
  if (ioctl(gpufd, KBASE_IOCTL_SET_FLAGS, &init_flags) < 0)
    LOG_FATAL("Error initializing GPU context! %s\n", strerror(errno));

  struct kbase_ioctl_hwcnt_reader_setup reader_setup;
  // Note: we don't actually ever use more than 1 buffer, but we need the total
  // memory size to be at least 1 page or we trigger a bug in the Midgard
  // drivers where mmap thinks we passed in a |length| that was too long,
  // because it rounds |length| up to the nearest page.
  reader_setup.num_buffers = 16;
  reader_setup.job_manager_mask = 0xFFFFFFFF;
  reader_setup.shader_mask = 0xFFFFFFFF;
  reader_setup.tiler_mask = 0xFFFFFFFF;
  reader_setup.mmu_l2_mask = 0xFFFFFFFF;
  reader_fd = ioctl(gpufd, KBASE_IOCTL_HWCNT_READER_SETUP, &reader_setup);
  if (reader_fd < 0)
    LOG_FATAL("Error setting up hwcnt reader! %s\n", strerror(errno));

  uint32_t api_version;
  if (ioctl(reader_fd, KBASE_HWCNT_READER_GET_API_VERSION, &api_version) < 0)
    LOG_FATAL("Error getting API version! %s\n", strerror(errno));
  assert(api_version >= SUPPORTED_API_VERSION);

  uint32_t hw_version;
  if (ioctl(reader_fd, KBASE_HWCNT_READER_GET_HWVER, &hw_version) < 0)
    LOG_FATAL("Error getting hardware version! %s\n", strerror(errno));
  assert(hw_version >= SUPPORTED_HW_VERSION);
}

void cleanup_mali_perf_reader() {
  close(gpufd);
  close(reader_fd);
}

struct mali_counter_values get_counter_values_from_dump(
    MaliGpuCounter counter,
    uint32_t* dump,
    size_t dump_size_bytes) {
  struct mali_counter_values ret;
  memset(&ret, 0, sizeof(struct mali_counter_values));

  if (counter >> 8 != curr_model) {
    LOG_ERROR("Error: counter is of incorrect model type!\n");
    return ret;
  }

  int lower_range = -1;
  int upper_range = -1;
  int counter_type = (counter >> 6) & 3;
  switch (counter_type) {
    case gpu_counter_job_manager:
      lower_range = 0;
      upper_range = 1;
      break;
    case gpu_counter_tiler:
      lower_range = 1;
      upper_range = 2;
      break;
    case gpu_counter_l2:
      lower_range = 2;
      upper_range = lower_range + num_l2_caches;
      break;
    case gpu_counter_shader:
      lower_range = 2 + num_l2_caches;
      upper_range = lower_range + num_shader_cores;
      break;
    default:
      // This should never happen
      break;
  }

  ret.counter = counter;
  ret.num_values = upper_range - lower_range;
  ret.values = (uint32_t*)malloc(ret.num_values * sizeof(uint32_t));

  int index = 0;
  int present_index = -1;
  const int max_index = dump_size_bytes / (64 * sizeof(uint32_t));
  while (index < max_index && present_index < upper_range) {
    if (dump[index * 64 + 2])
      present_index++;

    if (present_index >= lower_range && present_index < upper_range) {
      uint32_t counter_val = dump[index * 64 + (counter & 0x3F)];
      ret.values[present_index - lower_range] = counter_val;
    }

    index++;
  }

  return ret;
}

size_t get_dump_size_bytes() {
  uint32_t ret;
  if (ioctl(reader_fd, KBASE_HWCNT_READER_GET_BUFFER_SIZE, &ret) < 0)
    LOG_ERROR("Error reading dump buffer size! %s\n", strerror(errno));
  return ret;
}

void reset_perf_metrics() {
  if (ioctl(reader_fd, KBASE_HWCNT_READER_CLEAR, NULL) < 0)
    LOG_ERROR("Error clearing dump buffer! %s\n", strerror(errno));
}

void initiate_dump() {
  if (ioctl(reader_fd, KBASE_HWCNT_READER_DUMP, NULL) < 0)
    LOG_ERROR("Error dumping performance metrics! %s\n", strerror(errno));
}

void get_dump_buffer() {
  if (ioctl(reader_fd, KBASE_HWCNT_READER_GET_BUFFER, &reader_metadata) < 0)
    LOG_ERROR("Error getting dump buffer! %s\n", strerror(errno));
}

void put_dump_buffer() {
  if (ioctl(reader_fd, KBASE_HWCNT_READER_PUT_BUFFER, &reader_metadata) < 0)
    LOG_ERROR("Error putting dump buffer! %s\n", strerror(errno));
}

uint8_t* copy_dump_to_userspace(size_t dump_size_bytes) {
  uint8_t* ret = (uint8_t*)malloc(dump_size_bytes);

  int offset = 0;
  const int kMaxWindowSize = 4096;
  while (offset < dump_size_bytes) {
    int curr_window_size = dump_size_bytes - offset < kMaxWindowSize
                               ? (dump_size_bytes - offset)
                               : kMaxWindowSize;

    uint8_t* curr_window = (uint8_t*)mmap(NULL, curr_window_size, PROT_READ,
                                          MAP_SHARED, reader_fd, offset);
    if (curr_window == MAP_FAILED) {
      LOG_ERROR("Error mapping dump buffer! %s\n", strerror(errno));
      break;
    }

    memcpy(ret + offset, curr_window, curr_window_size);

    munmap(curr_window, curr_window_size);

    offset += kMaxWindowSize;
  }

  return ret;
}

struct mali_counter_response read_perf_metrics(MaliGpuCounter* counters,
                                               size_t num_counters) {
  initiate_dump();
  get_dump_buffer();

  size_t dump_size_bytes = get_dump_size_bytes();
  uint8_t* dump_data = copy_dump_to_userspace(dump_size_bytes);

  put_dump_buffer();

  struct mali_counter_response ret;
  ret.num_counters = num_counters;
  ret.counter_values = (struct mali_counter_values*)malloc(
      num_counters * sizeof(struct mali_counter_values));
  for (int i = 0; i < num_counters; i++) {
    ret.counter_values[i] = get_counter_values_from_dump(
        counters[i], (uint32_t*)dump_data, dump_size_bytes);
  }

  free(dump_data);

  return ret;
}

void free_counters(struct mali_counter_response counters) {
  for (int i = 0; i < counters.num_counters; i++) {
    free(counters.counter_values[i].values);
  }
  free(counters.counter_values);
}
