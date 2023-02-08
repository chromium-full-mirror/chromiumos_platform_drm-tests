/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __BITSTREAM_HELPER_H265_H__
#define __BITSTREAM_HELPER_H265_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool init_bitstream_h265(void);

uint32_t get_fourcc_h265(void);

bool is_end_of_stream_h265(void);

size_t fill_compressed_buffer_h265(uint8_t* dest, size_t max_len);

#endif  // BITSTREAM_HELPER_H265_H
