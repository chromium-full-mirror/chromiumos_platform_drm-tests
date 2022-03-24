/*
 * Copyright 2022 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bitstreams/bitstream_helper.h"

#include <stdio.h>
#include <sys/mman.h>

#include "bitstreams/bitstream_helper_h264.h"
#include "bitstreams/bitstream_helper_h265.h"
#include "bitstreams/bitstream_helper_ivf.h"
#include "v4l2_macros.h"

uint8_t* file_buf = NULL;
uint64_t filesize = 0;
FILE* fd = NULL;
uint64_t curr_pos = 0;

uint32_t (*get_fourcc)(void) = NULL;

bool (*is_end_of_stream)(void) = NULL;

size_t (*fill_compressed_buffer)(uint8_t* dest, size_t max_len) = NULL;

bool init_bitstream(char* filename) {
  fd = fopen(filename, "rb");

  if (!fd) {
    LOG_ERROR("Unable to open file %s", filename);
    cleanup_bitstream();
    return false;
  }

  fseek(fd, 0, SEEK_END);
  filesize = ftell(fd);

  file_buf =
      (uint8_t*)mmap(NULL, filesize, PROT_READ, MAP_SHARED, fileno(fd), 0);

  if (init_bitstream_h264()) {
    LOG_INFO("Initializing H264 bitstream");

    get_fourcc = get_fourcc_h264;
    is_end_of_stream = is_end_of_stream_h264;
    fill_compressed_buffer = fill_compressed_buffer_h264;

    return true;
  } else if (init_bitstream_h265()) {
    LOG_INFO("Initializing H265 bitstream");

    get_fourcc = get_fourcc_h265;
    is_end_of_stream = is_end_of_stream_h265;
    fill_compressed_buffer = fill_compressed_buffer_h265;

    return true;
  } else if (init_bitstream_ivf()) {
    LOG_INFO("Initializing VP8/VP9 bitstream");
    get_fourcc = get_fourcc_ivf;
    is_end_of_stream = is_end_of_stream_ivf;
    fill_compressed_buffer = fill_compressed_buffer_ivf;

    return true;
  }

  LOG_ERROR("Error: Bitstream is of unknown format or malformed.");
  cleanup_bitstream();
  return false;
}

void cleanup_bitstream(void) {
  if (file_buf) {
    munmap(file_buf, filesize);
    file_buf = NULL;
    filesize = 0;
  }

  if (fd) {
    fclose(fd);
    fd = NULL;
  }
}
