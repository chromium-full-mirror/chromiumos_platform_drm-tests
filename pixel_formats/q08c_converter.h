/*
 * Copyright 2024 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stddef.h>
#include <stdint.h>

#ifndef __Q08C_CONVERTER_H__
#define __Q08C_CONVERTER_H__

void convert_q08c_to_nv12(uint8_t* dest_y,
                          uint8_t* dest_uv,
                          const uint8_t* src,
                          size_t width,
                          size_t height);

#endif
