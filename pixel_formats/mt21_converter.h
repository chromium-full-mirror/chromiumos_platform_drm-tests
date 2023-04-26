/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stddef.h>
#include <stdint.h>

#ifndef __MT21_CONVERTER_H__
#define __MT21_CONVERTER_H__

void convert_mt21_to_nv12(uint8_t* dest_y,
                          uint8_t* dest_uv,
                          const uint8_t* src_y,
                          const uint8_t* src_uv,
                          size_t width,
                          size_t height,
                          size_t y_buf_len,
                          size_t uv_buf_len);

size_t compute_mt21_min_y_size(size_t width, size_t height);

size_t compute_mt21_min_uv_size(size_t width, size_t height);

#endif
