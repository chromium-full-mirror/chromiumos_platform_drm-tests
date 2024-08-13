/*
 * Copyright 2022 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef V4L2_MACROS_H
#define V4L2_MACROS_H

#include <linux/videodev2.h>

#include "logging.h"

#define FOURCC_SIZE 4

// TODO(frkoenig):
// P010 had not landed as an official V4L2 format yet. Once it has remove this.
#ifndef V4L2_PIX_FMT_P010
#define V4L2_PIX_FMT_P010 \
  v4l2_fourcc('P', '0', '1', '0') /* 15  Y/CbCr 4:2:0 10-bit per pixel*/
#endif
// Only available in kernels 5.4 and later.
#ifndef V4L2_PIX_FMT_NV12_UBWC
#define V4L2_PIX_FMT_NV12_UBWC \
  v4l2_fourcc('Q', '1', '2', '8') /* UBWC 8-bit Y/CbCr 4:2:0  */
#endif

#define V4L2_PIX_FMT_INVALID 0

#endif  // V4L2_MACROS_H
