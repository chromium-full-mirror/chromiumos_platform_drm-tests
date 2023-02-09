/*
 * Copyright 2023 The ChromiumOS Authors
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

size_t (*fill_compressed_buffer)(uint8_t* dest,
                                 size_t max_len,
                                 struct bitstream* bts) = NULL;

bool init_bitstream(const char* filename, struct bitstream* bts) {
  bts->fd = fopen(filename, "rb");

  if (!bts->fd) {
    LOG_ERROR("Unable to open file %s", filename);
    cleanup_bitstream(bts);
    return false;
  }

  fseek(bts->fd, 0, SEEK_END);
  bts->filesize = ftell(bts->fd);

  bts->file_buf =
      (uint8_t*)mmap(NULL, bts->filesize, PROT_READ, MAP_SHARED,
                     fileno(bts->fd), 0);

  bts->eof = bts->file_buf + bts->filesize;

  if (init_bitstream_h264(bts)) {
    LOG_INFO("Initializing H264 bitstream");
    fill_compressed_buffer = fill_compressed_buffer_h264;

    return true;
  } else if (init_bitstream_h265(bts)) {
    LOG_INFO("Initializing H265 bitstream");
    fill_compressed_buffer = fill_compressed_buffer_h265;

    return true;
  } else if (init_bitstream_ivf(bts)) {
    LOG_INFO("Initializing VP8/VP9 bitstream");
    fill_compressed_buffer = fill_compressed_buffer_ivf;

    return true;
  }

  LOG_ERROR("Error: Bitstream is of unknown format or malformed.");
  cleanup_bitstream(bts);
  return false;
}

void cleanup_bitstream(struct bitstream* bts) {
  if (bts->file_buf) {
    munmap(bts->file_buf, bts->filesize);
    bts->file_buf = NULL;
    bts->filesize = 0;
  }

  if (bts->fd) {
    fclose(bts->fd);
    bts->fd = NULL;
  }

  if (bts->private) {
    free(bts->private);
    bts->private = NULL;
  }
}

bool is_end_of_stream(struct bitstream* bts) {
  return bts->curr_pos >= bts->eof;
}
