/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __BITSTREAM_HELPER_H264_H__
#define __BITSTREAM_HELPER_H264_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool init_bitstream_h264(void);

uint32_t get_fourcc_h264(void);

bool is_end_of_stream_h264(void);

size_t fill_compressed_buffer_h264(uint8_t* dest, size_t max_len);

#endif  // BITSTREAM_HELPER_H264_H
