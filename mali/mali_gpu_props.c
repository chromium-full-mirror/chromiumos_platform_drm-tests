/*
 * Copyright 2022 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "mali/mali_gpu_props.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "logging.h"
#include "mali/mali_ioctl.h"

#define PROPS_BUFFER_SIZE 4096
char props_buffer_cached = 0;
uint8_t props_buffer[PROPS_BUFFER_SIZE];

uint64_t get_cached_gpu_prop(MaliGpuProperty prop) {
  int i = 0;
  uint64_t ret;
  while (i + 12 < PROPS_BUFFER_SIZE) {
    uint32_t key = *(uint32_t*)(props_buffer + i);
    if (!key)
      break;

    i += 4;
    int size_type = key & 0x3;
    int key_type = key >> 2;

    switch (size_type) {
      case 0:
        ret = *(uint8_t*)(props_buffer + i);
        i += 1;
        break;
      case 1:
        ret = *(uint16_t*)(props_buffer + i);
        i += 2;
        break;
      case 2:
        ret = *(uint32_t*)(props_buffer + i);
        i += 4;
        break;
      case 3:
        ret = *(uint64_t*)(props_buffer + i);
        i += 8;
        break;
      default:
        LOG_ERROR(
            "Error: GPU properties buffer contains malformed key at index "
            "%d.\n",
            i - 4);
        return -1;
    }

    if (key_type == prop)
      return ret;
  }

  LOG_ERROR("Error: GPU property %d not found.\n", prop);
  return -1;
}

void get_gpu_property_buffer() {
  int gpufd = open(kGpuDevice, O_RDWR | O_CLOEXEC);
  if (gpufd < 0)
    LOG_FATAL("Error opening GPU device! %s\n", strerror(errno));

  // Note that even though we don't strictly speaking need this information for
  // querying GPU properties, the GPU properties IOCTL will fail unless we check
  // the version and set the flags.
  struct kbase_ioctl_version_check version_check;
  if (ioctl(gpufd, KBASE_IOCTL_VERSION_CHECK, &version_check) < 0) {
    if (errno != EPERM ||
        ioctl(gpufd, KBASE_IOCTL_VERSION_CHECK_CSF, &version_check) < 0) {
      LOG_FATAL("Error checking GPU version! %s\n", strerror(errno));
    } else {
      assert(version_check.major >= SUPPORTED_MAJOR_VERSION_CSF);
      assert(version_check.minor >= SUPPORTED_MINOR_VERSION);
    }
  } else {
    assert(version_check.major >= SUPPORTED_MAJOR_VERSION);
    assert(version_check.minor >= SUPPORTED_MINOR_VERSION);
  }

  struct kbase_ioctl_set_flags init_flags;
  init_flags.create_flags = BASE_CONTEXT_SYSTEM_MONITOR_SUBMIT_DISABLED;
  if (ioctl(gpufd, KBASE_IOCTL_SET_FLAGS, &init_flags) < 0)
    LOG_FATAL("Error initializing GPU context! %s\n", strerror(errno));

  struct kbase_ioctl_get_gpuprops gpu_props_req;
  gpu_props_req.buffer_ptr = (uint64_t)props_buffer;
  gpu_props_req.size = PROPS_BUFFER_SIZE;
  gpu_props_req.flags = 0;
  if (ioctl(gpufd, KBASE_IOCTL_GET_GPUPROPS, &gpu_props_req) < 0)
    LOG_FATAL("Error getting GPU properties! %s\n", strerror(errno));

  props_buffer_cached = 1;

  close(gpufd);
}

uint64_t get_gpu_prop(MaliGpuProperty prop) {
  if (!props_buffer_cached)
    get_gpu_property_buffer();

  return get_cached_gpu_prop(prop);
}
