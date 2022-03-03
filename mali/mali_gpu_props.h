/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdint.h>

#ifndef __MALI_GPU_PROPS_H__
#define __MALI_GPU_PROPS_H__

// Constant values found in
// third_party/kernel/next/drivers/gpu/arm/valhall/mali_kbase_ioctl.h
typedef enum {
  gpu_prop_product_id = 1,
  gpu_prop_version_status = 2,
  gpu_prop_minor_revision = 3,
  gpu_prop_major_revision = 4,
  gpu_prop_freq_max_khz = 6,
  gpu_prop_log2_program_counter_size = 8,
  gpu_prop_texture_features_0 = 9,
  gpu_prop_texture_features_1 = 10,
  gpu_prop_texture_features_2 = 11,
  gpu_prop_available_memory_size = 12,
  gpu_prop_log2_l2_line_size = 13,
  gpu_prop_log2_l2_cache_size = 14,
  gpu_prop_num_l2 = 15,
  gpu_prop_tiler_bin_size_bytes = 16,
  gpu_prop_tiler_max_active_levels = 17,
  gpu_prop_max_threads = 18,
  gpu_prop_max_workgroup_size = 19,
  gpu_prop_max_barrier_size = 20,
  gpu_prop_max_registers = 21,
  gpu_prop_max_task_queue = 22,
  gpu_prop_max_thread_group_split = 23,
  gpu_prop_impl_tech = 24,
  gpu_prop_shader_present_mask = 25,
  gpu_prop_tiler_present = 26,
  gpu_prop_l2_present = 27,
  gpu_prop_stack_present = 28,
  gpu_prop_l2_features = 29,
  gpu_prop_core_features = 30,
  gpu_prop_mem_features = 31,
  gpu_prop_mmu_features = 32,
  gpu_prop_as_present = 33,
  gpu_prop_js_present = 34,
  gpu_prop_js_features_0 = 35,
  gpu_prop_js_features_1 = 36,
  gpu_prop_js_features_2 = 37,
  gpu_prop_js_features_3 = 38,
  gpu_prop_js_features_4 = 39,
  gpu_prop_js_features_5 = 40,
  gpu_prop_js_features_6 = 41,
  gpu_prop_js_features_7 = 42,
  gpu_prop_js_features_8 = 43,
  gpu_prop_js_features_9 = 44,
  gpu_prop_js_features_10 = 45,
  gpu_prop_js_features_11 = 46,
  gpu_prop_js_features_12 = 47,
  gpu_prop_js_features_13 = 48,
  gpu_prop_js_features_14 = 49,
  gpu_prop_js_features_15 = 50,
  gpu_prop_tiler_features = 51,
  gpu_prop_raw_texture_features_0 = 52,
  gpu_prop_raw_texture_features_1 = 53,
  gpu_prop_raw_texture_features_2 = 54,
  gpu_prop_gpu_id = 55,
  gpu_prop_raw_max_threads = 56,
  gpu_prop_raw_max_workgroupd_size = 57,
  gpu_prop_raw_max_barrier_size = 58,
  gpu_prop_thread_features = 59,
  gpu_prop_coherency_mode = 60,
  gpu_prop_num_coherency_groups = 61,
  gpu_prop_num_core_coherency_groups = 62,
  gpu_prop_coherency = 63,
  gpu_prop_coherency_group_0 = 64,
  gpu_prop_coherency_group_1 = 65,
  gpu_prop_coherency_group_2 = 66,
  gpu_prop_coherency_group_3 = 67,
  gpu_prop_coherency_group_4 = 68,
  gpu_prop_coherency_group_5 = 69,
  gpu_prop_coherency_group_6 = 70,
  gpu_prop_coherency_group_7 = 71,
  gpu_prop_coherency_group_8 = 72,
  gpu_prop_coherency_group_9 = 73,
  gpu_prop_coherency_group_10 = 74,
  gpu_prop_coherency_group_11 = 75,
  gpu_prop_coherency_group_12 = 76,
  gpu_prop_coherency_group_13 = 77,
  gpu_prop_coherency_group_14 = 78,
  gpu_prop_coherency_group_15 = 79,
  gpu_prop_texture_features_3 = 80,
  gpu_prop_raw_texture_features_3 = 81,
  gpu_prop_num_exec_engines = 82,
  gpu_prop_raw_thread_tls_alloc = 83,
  gpu_prop_tls_alloc = 84
} MaliGpuProperty;

uint64_t get_gpu_prop(MaliGpuProperty prop);

#endif  // __MALI_GPU_PROPS_H__
