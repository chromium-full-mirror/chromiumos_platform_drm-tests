/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __BITSTREAM_HELPER_IVF_H__
#define __BITSTREAM_HELPER_IVF_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool init_bitstream_ivf(void);

uint32_t get_fourcc_ivf(void);

bool is_end_of_stream_ivf(void);

size_t fill_compressed_buffer_ivf(uint8_t* dest, size_t max_len);

#endif  // BITSTREAM_HELPER_IVF_H
