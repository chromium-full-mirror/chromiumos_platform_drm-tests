/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <asm-generic/ioctl.h>
#include <stdint.h>

#ifndef __MALI_IOCTL_H__
#define __MALI_IOCTL_H__

static const char* kGpuDevice = "/dev/mali0";

// Definitions courtesy of
// third_party/kernel/v5.10/drivers/gpu/arm/valhall/mali_kbase_ioctl.h

#define KBASE_IOCTL_TYPE 0x80
#define KBASE_IOCTL_GET_GPUPROPS \
  _IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)
#define KBASE_IOCTL_SET_FLAGS \
  _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)
#define KBASE_IOCTL_VERSION_CHECK \
  _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_HWCNT_READER_SETUP \
  _IOW(KBASE_IOCTL_TYPE, 8, struct kbase_ioctl_hwcnt_reader_setup)

#define BASE_CONTEXT_SYSTEM_MONITOR_SUBMIT_DISABLED ((uint32_t)1 << 1)

struct __attribute__((packed)) kbase_ioctl_get_gpuprops {
  uint64_t buffer_ptr;
  uint32_t size;
  uint32_t flags;
};

struct __attribute__((packed)) kbase_ioctl_set_flags {
  uint32_t create_flags;
};

struct __attribute__((packed)) kbase_ioctl_version_check {
  uint16_t major;
  uint16_t minor;
};

struct __attribute__((packed)) kbase_ioctl_hwcnt_reader_setup {
  uint32_t num_buffers;
  uint32_t job_manager_mask;
  uint32_t shader_mask;
  uint32_t tiler_mask;
  uint32_t mmu_l2_mask;
};

// Definitions courtesy of
// third_party/kernel/v5.10/drivers/gpu/arm/valhall/mali_kbase_hwcnt_reader.h

#define KBASE_HWCNT_READER 0xBE
#define KBASE_HWCNT_READER_GET_HWVER _IOR(KBASE_HWCNT_READER, 0x00, uint32_t)
#define KBASE_HWCNT_READER_GET_BUFFER_SIZE \
  _IOR(KBASE_HWCNT_READER, 0x01, uint32_t)
#define KBASE_HWCNT_READER_DUMP _IOW(KBASE_HWCNT_READER, 0x10, uint32_t)
#define KBASE_HWCNT_READER_CLEAR _IOW(KBASE_HWCNT_READER, 0x11, uint32_t)
#define KBASE_HWCNT_READER_GET_BUFFER \
  _IOR(KBASE_HWCNT_READER, 0x20, struct kbase_hwcnt_reader_metadata)
#define KBASE_HWCNT_READER_PUT_BUFFER \
  _IOW(KBASE_HWCNT_READER, 0x21, struct kbase_hwcnt_reader_metadata)
#define KBASE_HWCNT_READER_SET_INTERVAL _IOW(KBASE_HWCNT_READER, 0x30, uint32_t)
#define KBASE_HWCNT_READER_ENABLE_EVENT _IOW(KBASE_HWCNT_READER, 0x40, uint32_t)
#define KBASE_HWCNT_READER_DISABLE_EVENT \
  _IOW(KBASE_HWCNT_READER, 0x41, uint32_t)
#define KBASE_HWCNT_READER_GET_API_VERSION \
  _IOW(KBASE_HWCNT_READER, 0xFF, uint32_t)

struct kbase_hwcnt_reader_metadata {
  uint64_t timestamp;
  uint32_t event_id;
  uint32_t buffer_idx;
};

#define SUPPORTED_MAJOR_VERSION 11
#define SUPPORTED_MINOR_VERSION 21
#define SUPPORTED_API_VERSION 1
#define SUPPORTED_HW_VERSION 5

#endif  // __MALI_IOCTL_H__
