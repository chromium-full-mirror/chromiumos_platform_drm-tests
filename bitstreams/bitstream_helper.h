/*
 * Copyright 2023 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef __BITSTREAM_HELPER_H__
#define __BITSTREAM_HELPER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct bitstream {
  // mmap'd buffer of the currently opened encoded file.
  uint8_t* file_buf;
  // Size of the currently opened encoded file.
  uint64_t filesize;
  // Current position of the decode in the encoded file.
  uint8_t* curr_pos;
  // Pointer to the end of the buffer for length checking.
  uint8_t* eof;
  // pointer to file containing bitstream
  FILE* fd;
  // fourcc derived from the bitstream metadata
  uint32_t fourcc;
  // per codec information
  void* private;
};

// Initializes the bitstream and parses its magic header to figure out what type
// of file it is.
bool init_bitstream(const char* filename, struct bitstream* bts);

// Cleans up the bitstream helper.
void cleanup_bitstream(struct bitstream* bts);

// Returns whether or not we have reached the end of the stream.
bool is_end_of_stream(struct bitstream* bts);

// Fills an OUTPUT queue buffer (which is V4L2 terminology for encoded data
// buffer) with frame data up to |max_len| bytes.
extern size_t (*fill_compressed_buffer)(uint8_t* dest,
                                        size_t max_len,
                                        struct bitstream* bts);

#endif
