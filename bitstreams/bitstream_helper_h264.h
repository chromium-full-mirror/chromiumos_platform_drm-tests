/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __BITSTREAM_HELPER_H264_H__
#define __BITSTREAM_HELPER_H264_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bitstream_helper.h"

bool init_bitstream_h264(struct bitstream* bitstream);

size_t fill_compressed_buffer_h264(uint8_t* dest,
                                   size_t max_len,
                                   struct bitstream* bts);

#endif  // BITSTREAM_HELPER_H264_H
