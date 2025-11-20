/*
 * Copyright 2022 The ChromiumOS Authors
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
#define KBASE_IOCTL_VERSION_CHECK_CSF \
  _IOWR(KBASE_IOCTL_TYPE, 52, struct kbase_ioctl_version_check)
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

// New prfcnt API.
#define PRFCNT_MODE_MANUAL 0
#define PRFCNT_MODE_PERIODIC 1
#define PRFCNT_BLOCK_TYPE_FRONTEND 0
#define PRFCNT_BLOCK_TYPE_TILER 1
#define PRFCNT_BLOCK_TYPE_MEMORY 2
#define PRFCNT_BLOCK_TYPE_SHADER_CORE 3
#define PRFCNT_SET_PRIMARY 0
#define PRFCNT_SET_SECONDARY 1
#define PRFCNT_SET_TERTIARY 2
#define PRFCNT_SCOPE_GLOBAL 0
#define PRFCNT_REQUEST_TYPE_MODE 0x1000
#define PRFCNT_REQUEST_TYPE_ENABLE 0x1001
#define PRFCNT_REQUEST_TYPE_SCOPE 0x1002
#define PRFCNT_METADATA_TYPE_SAMPLE 0x2000
#define PRFCNT_METADATA_TYPE_CLOCK 0x2001
#define PRFCNT_METADATA_TYPE_BLOCK 0x2002
#define PRFCNT_CONTROL_CMD_START 1
#define PRFCNT_CONTROL_CMD_STOP 2
#define PRFCNT_CONTROL_CMD_SAMPLE_SYNC 3
#define PRFCNT_CONTROL_CMD_DISCARD 5

struct __attribute__((packed)) prfcnt_request_mode {
  uint8_t mode;
  uint8_t pad[7];
  uint64_t period_ns;
};

struct __attribute__((packed)) prfcnt_request_enable {
  uint8_t block_type;
  uint8_t set;
  uint8_t pad[6];
  uint64_t enable_mask[2];
};

struct __attribute__((packed)) prfcnt_request_scope {
  uint8_t scope;
  uint8_t pad[7];
};

struct __attribute__((packed)) prfcnt_request_item {
  uint16_t item_type;
  uint16_t item_version;
  uint8_t pad[4];
  union {
    struct prfcnt_request_mode req_mode;
    struct prfcnt_request_enable req_enable;
    struct prfcnt_request_scope req_scope;
  } payload;
};

union __attribute__((packed)) kbase_ioctl_kinstr_prfcnt_setup {
  struct {
    uint32_t req_item_count;
    uint32_t req_item_size;
    uint64_t req_array_ptr;
  } in;
  struct {
    uint32_t metadata_item_size;
    uint32_t mmap_size_bytes;
  } out;
};

struct __attribute__((packed)) prfcnt_sample_metadata {
  uint64_t timestamp_start;
  uint64_t timestamp_end;
  uint64_t seq;
  uint64_t user_data;
  uint32_t flags;
  uint32_t pad;
};

struct __attribute__((packed)) prfcnt_clock_metadata {
  uint32_t num_domains;
  uint32_t pad;
  uint64_t cycles[4];
};

struct __attribute__((packed)) prfcnt_block_metadata {
  uint8_t block_type;
  uint8_t block_idx;
  uint8_t set;
  uint8_t pad;
  uint32_t block_state;
  uint32_t values_offset;
  uint32_t pad2;
};

struct __attribute__((packed)) prfcnt_metadata {
  uint16_t item_type;
  uint16_t item_version;
  uint8_t padding[4];
  union {
    struct prfcnt_sample_metadata sample_metadata;
    struct prfcnt_clock_metadata clock_metadata;
    struct prfcnt_block_metadata block_metadata;
  } u;
};

struct __attribute__((packed)) prfcnt_control_cmd {
  uint16_t cmd;
  uint16_t pad[3];
  uint64_t user_data;
};

struct __attribute__((packed)) prfcnt_sample_access {
  uint64_t sequence_number;
  uint64_t metadata_list_offset;
};

#define KBASE_IOCTL_KINSTR_PRFCNT_SETUP \
  _IOWR(KBASE_IOCTL_TYPE, 57, union kbase_ioctl_kinstr_prfcnt_setup)

#define KBASE_KINSTR_PRFCNT_READER 0xBF
#define KBASE_IOCTL_KINSTR_PRFCNT_CMD \
  _IOW(KBASE_KINSTR_PRFCNT_READER, 0x00, struct prfcnt_control_cmd)
#define KBASE_IOCTL_KINSTR_PRFCNT_GET_SAMPLE \
  _IOR(KBASE_KINSTR_PRFCNT_READER, 0x01, struct prfcnt_sample_access)
#define KBASE_IOCTL_KINSTR_PRFCNT_PUT_SAMPLE \
  _IOW(KBASE_KINSTR_PRFCNT_READER, 0x10, struct prfcnt_sample_access)

#define SUPPORTED_MAJOR_VERSION 11
#define MINOR_VERSION_NEW_COUNTERS 40
#define SUPPORTED_MAJOR_VERSION_CSF 1
#define SUPPORTED_MINOR_VERSION 11
#define SUPPORTED_API_VERSION 1
#define SUPPORTED_HW_VERSION 5

#endif  // __MALI_IOCTL_H__
