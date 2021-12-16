/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __BITSTREAM_HELPER_H__
#define __BITSTREAM_HELPER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Initializes the bitstream and parses its magic header to figure out what type
// of file it is.
bool init_bitstream(char* filename);

// Cleans up the bitstream helper.
void cleanup_bitstream(void);

// mmap'd buffer of the currently opened encoded file.
extern uint8_t* file_buf;

// Size of the currently opened encoded file.
extern uint64_t filesize;

// Current position of the decode in the encoded file.
extern uint64_t curr_pos;

// Returns the fourcc code for the bitstream.
extern uint32_t (*get_fourcc)(void);

// Returns whether or not we have reached the end of the stream.
extern bool (*is_end_of_stream)(void);

// Fills an OUTPUT queue buffer (which is V4L2 terminology for encoded data
// buffer) with frame data up to |max_len| bytes.
extern size_t (*fill_compressed_buffer)(uint8_t* dest, size_t max_len);

#endif
