/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * This test evaluates the speed at which BOs of various USE flags can be
 * accessed when mmap()ped. To represent Chrome graphics buffers uses, a naive
 * rotation operation is implemented here in C90. This also factors out the use
 * or not of SIMD instructions and/or sophisticated access patterns like those
 * employed by libyuv: this is OK here since we're only interested in relative
 * measurements comparing one BO USE flag set with another.
 * See https://tinyurl.com/cros-video-capture-buffers and b/169302186 for more
 * context.
 */

#include <assert.h>
#include <getopt.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <time.h>

#include "bs_drm.h"

#define HANDLE_EINTR_AND_EAGAIN(x)                                 \
  ({                                                               \
    int result;                                                    \
    do {                                                           \
      result = (x);                                                \
    } while (result != -1 && (errno == EINTR || errno == EAGAIN)); \
    result;                                                        \
  })

int dma_sync(int fd, __u64 flags) {
  struct dma_buf_sync sync_point = {0};
  sync_point.flags = flags;
  return HANDLE_EINTR_AND_EAGAIN(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync_point));
}

// N.B. This function actually does a clockwise 90-degree rotation and then a
// horizontal flip.
void NV12Rotate90(const uint8_t* src_y, int src_stride_y,
                  const uint8_t* src_uv, int src_stride_uv,
                  uint8_t* dst_y, int dst_stride_y,
                  uint8_t* dst_uv, int dst_stride_uv,
                  int src_width, int src_height) {
  // This loop walks the |src_y| samples in scanout order, but writes them in
  // the rotated order, hence doing big jumps in the destination space.
  for (int row = 0; row < src_height; ++row) {
    for (int col = 0; col < src_width; ++col) {
      const uint8_t* src_sample = src_y + row * src_stride_y + col;
      *(dst_y + col * dst_stride_y + row) = *src_sample;
    }
  }
  // Same idea but note the halving of |src_height| and |src_width| for the UV
  // planes, and treat |src_uv| and |dst_uv| as uint16_t arrays to account for
  // the UV pairs.
  const int uv_src_height = (src_height + 1) / 2;
  const int uv_src_width = (src_width + 1) / 2;
  for (int row = 0; row < uv_src_height; ++row) {
    for (int col = 0; col < uv_src_width; ++col) {
      const uint16_t* src_sample =
          (const uint16_t*)src_uv + row * (src_stride_uv / 2) + col;
      *((uint16_t*)dst_uv + col * (dst_stride_uv / 2) + row) = *src_sample;
    }
  }
}


struct test_case {
  uint32_t format; /* format for allocating buffer object from GBM */
  enum gbm_bo_transfer_flags read_write;
  enum gbm_bo_flags usage;
};

static void print_format_and_use_flags(FILE* out,
                                       const struct test_case* tcase) {
  fprintf(out, "format: ");
  switch (tcase->format) {
    case GBM_FORMAT_NV12:
      fprintf(out, "GBM_FORMAT_NV12");
      break;
    default:
      fprintf(out, "GBM_FORMAT_????????");
  }

  fprintf(out, ", access: %s%s",
          (tcase->read_write & GBM_BO_TRANSFER_READ ? "R" : ""),
          (tcase->read_write & GBM_BO_TRANSFER_WRITE ? "W" : ""));

  fprintf(out, ", use flags: ");
  bool first = true;
  if (tcase->usage & GBM_BO_USE_SCANOUT) {
    fprintf(out, "%sGBM_BO_USE_SCANOUT", first ? "" : " | ");
    first = false;
  }
  if (tcase->usage & GBM_BO_USE_LINEAR) {
    fprintf(out, "%sGBM_BO_USE_LINEAR", first ? "" : " | ");
    first = false;
  }
  if (tcase->usage & GBM_BO_USE_TEXTURING) {
    fprintf(out, "%sGBM_BO_USE_TEXTURING", first ? "" : " | ");
    first = false;
  }
  if (tcase->usage & GBM_BO_USE_CAMERA_READ) {
    fprintf(out, "%sGBM_BO_USE_CAMERA_READ", first ? "" : " | ");
    first = false;
  }
  if (tcase->usage & GBM_BO_USE_CAMERA_WRITE) {
    fprintf(out, "%sGBM_BO_USE_CAMERA_WRITE", first ? "" : " | ");
    first = false;
  }
  if (tcase->usage & GBM_BO_USE_SW_READ_OFTEN) {
    fprintf(out, "%sGBM_BO_USE_SW_READ_OFTEN", first ? "" : " | ");
    first = false;
  }
  if (tcase->usage & GBM_BO_USE_SW_WRITE_OFTEN) {
    fprintf(out, "%sGBM_BO_USE_SW_WRITE_OFTEN", first ? "" : " | ");
    first = false;
  }
}

static const struct option longopts[] = {
    {"help", no_argument, NULL, 'h'},
    {0, 0, 0, 0},
};

static void print_help(const char* argv0) {
  printf("Usage: %s [OPTIONS]\n", argv0);
  printf(" -h, --help     Print help.\n");
}

int main(int argc, char** argv) {
  // TODO(mcasas): Consider adding other formats/other operations.
  // TODO(mcasas): Transform this list into a cartesian product like GTest does.
  // TODO(mcasas): add command line flags to run test cases individually/by
  // groups, and to list them.
  const struct test_case tcases[] = {
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ, GBM_BO_USE_SCANOUT},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ, GBM_BO_USE_LINEAR},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ, GBM_BO_USE_TEXTURING},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ, GBM_BO_USE_CAMERA_READ},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ, GBM_BO_USE_CAMERA_WRITE},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ, GBM_BO_USE_SW_READ_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ, GBM_BO_USE_SW_WRITE_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SW_READ_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SW_WRITE_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SW_READ_OFTEN |
           GBM_BO_USE_SW_WRITE_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE, GBM_BO_USE_SCANOUT},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE, GBM_BO_USE_LINEAR},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE, GBM_BO_USE_TEXTURING},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE, GBM_BO_USE_CAMERA_READ},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE, GBM_BO_USE_CAMERA_WRITE},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE, GBM_BO_USE_SW_READ_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE, GBM_BO_USE_SW_WRITE_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SW_READ_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SW_WRITE_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SW_READ_OFTEN |
           GBM_BO_USE_SW_WRITE_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_WRITE, GBM_BO_USE_SCANOUT},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE, GBM_BO_USE_SCANOUT},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE, GBM_BO_USE_LINEAR},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE, GBM_BO_USE_TEXTURING},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE, GBM_BO_USE_CAMERA_READ},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE, GBM_BO_USE_CAMERA_WRITE},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE, GBM_BO_USE_SW_READ_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE, GBM_BO_USE_SW_WRITE_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SW_READ_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SW_WRITE_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE,
       GBM_BO_USE_LINEAR | GBM_BO_USE_SW_READ_OFTEN |
           GBM_BO_USE_SW_WRITE_OFTEN},
      {GBM_FORMAT_NV12, GBM_BO_TRANSFER_READ_WRITE, GBM_BO_USE_SCANOUT},
  };
  const size_t tcases_size = BS_ARRAY_LEN(tcases);

  // Make sure that the clock resolution is at least 1ms.
  struct timespec clock_resolution;
  clock_getres(CLOCK_MONOTONIC, &clock_resolution);
  assert(clock_resolution.tv_sec == 0 && clock_resolution.tv_nsec <= 1000000);

  int c;
  while ((c = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
    switch (c) {
      case 'h':
      default:
        print_help(argv[0]);
        return EXIT_SUCCESS;
    }
  }

  int display_fd = bs_drm_open_main_display();
  if (display_fd < 0) {
    bs_debug_error("failed to open card for display");
    return EXIT_FAILURE;
  }

  struct gbm_device* gbm = gbm_create_device(display_fd);
  if (!gbm) {
    bs_debug_error("failed to create gbm device");
    return EXIT_FAILURE;
  }

  // bs_mapper_dma_buf_new() is expected to use mmap().
  struct bs_mapper* mapper = bs_mapper_dma_buf_new();
  if (mapper == NULL) {
    bs_debug_error("failed to create mapper object");
    return EXIT_FAILURE;
  }

  const uint32_t width = 1920;
  const uint32_t height = 1080;

// We allocate NUM_BOS to replicate a bit what is done in video capture.
#define NUM_BOS 5
  struct gbm_bo* bos[NUM_BOS];
  uint8_t* ptr_y[NUM_BOS];
  uint8_t* ptr_uv[NUM_BOS];

  uint32_t stride_y[NUM_BOS];
  void* map_data_y[NUM_BOS];
  uint32_t stride_uv[NUM_BOS];
  void* map_data_uv[NUM_BOS];

#define NUM_PLANES 2
  int gbm_bo_fds[NUM_PLANES][NUM_BOS];
#define NUM_ITERS 10
  printf("Running %d iterations. %d BOs allocated (%dx%d)\n", NUM_ITERS,
         NUM_BOS, width, height);

  // |draft_canvas| is allocated as if to be an ARGB buffer, and can fit NV12
  // data of the same |width| and |height|.
  uint8_t* draft_canvas = malloc(width * height * 4);
  // This is not so much for clearing it as it is for accessing it once.
  memset(draft_canvas, 0, width * height * 4);

  for (size_t i = 0; i < tcases_size; i++) {
    const struct test_case* tcase = &tcases[i];
    print_format_and_use_flags(stdout, tcase);
    printf(": ");

    for (size_t j = 0; j < NUM_BOS; j++) {
      bos[j] = gbm_bo_create(gbm, width, height, tcase->format, tcase->usage);
      if (!bos[j]) {
        printf(
            "gbm_bo_create() failed (probably format or usage is not "
            "supported.\n");
        continue;
      }

      const int expected_num_planes = NUM_PLANES;
      const int num_planes = gbm_bo_get_plane_count(bos[j]);
      if (expected_num_planes != num_planes) {
        printf("Incorrect number of planes, expected %d, got %d\n",
               expected_num_planes, num_planes);
        return EXIT_FAILURE;
      }

      ptr_y[j] = bs_mapper_map(mapper, bos[j], 0, &map_data_y[j], &stride_y[j]);
      if (ptr_y[j] == MAP_FAILED) {
        bs_debug_error("failed to mmap gbm bo plane 0 (Y)");
        return EXIT_FAILURE;
      }

      ptr_uv[j] =
          bs_mapper_map(mapper, bos[j], 1, &map_data_uv[j], &stride_uv[j]);
      if (ptr_uv[j] == MAP_FAILED) {
        bs_debug_error("failed to mmap gbm bo plane 1 (UV)");
        return EXIT_FAILURE;
      }

      for (size_t plane = 0; plane < NUM_PLANES; plane++) {
        gbm_bo_fds[plane][j] = gbm_bo_get_fd_for_plane(bos[j], plane);
        if (gbm_bo_fds[plane][j] < 0) {
          bs_debug_error("failed to get BO fd");
          return EXIT_FAILURE;
        }
      }
    }

    struct timespec start, stop;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (size_t j = 0; j < NUM_ITERS; j++) {
      const uint32_t bo_index = j % NUM_BOS;

      if (tcase->read_write & GBM_BO_TRANSFER_READ) {
        assert(dma_sync(gbm_bo_fds[0][bo_index],
                        DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) == 0);
        assert(dma_sync(gbm_bo_fds[1][bo_index],
                        DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) == 0);

        // Typical Chrome access patterns like e.g. libyuv NV12ToARGB/NV12Scale
        // are asymmetric in the sense that they create scattered read/writes
        // (e.g. pixel packing/unpacking operations) or simply more of those on
        // either source or destination. A rotation operation is chosen here to
        // avoid part of that asymmetry.
        // TODO(mcasas): investigate other functions which might cause other
        // memory access patterns.
        NV12Rotate90(ptr_y[bo_index], stride_y[bo_index],
                     ptr_uv[bo_index], stride_uv[bo_index],
                     draft_canvas, height,
                     draft_canvas + (height * width), height,
                     width, height);

        assert(dma_sync(gbm_bo_fds[0][bo_index],
                        DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) == 0);
        assert(dma_sync(gbm_bo_fds[1][bo_index],
                        DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) == 0);
      }

      // When writing, use the next BO index so that nobody will try to optimize
      // the whole operation chain away when having READ-then-WRITE.
      const uint32_t next_bo_index = (j + 1) % NUM_BOS;
      if (tcase->read_write & GBM_BO_TRANSFER_WRITE) {
        assert(dma_sync(gbm_bo_fds[0][next_bo_index],
                        DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) == 0);
        assert(dma_sync(gbm_bo_fds[1][next_bo_index],
                        DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) == 0);

        // We pretend |draft_canvas| has portrait orientation, so the
        // destination of the rotation fits into a landscape orientation BO.
        NV12Rotate90(draft_canvas, height,
                     draft_canvas + (height * width), height,
                     ptr_y[bo_index], stride_y[bo_index],
                     ptr_uv[bo_index], stride_uv[bo_index],
                     height, width);

        assert(dma_sync(gbm_bo_fds[0][next_bo_index],
                        DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) == 0);
        assert(dma_sync(gbm_bo_fds[1][next_bo_index],
                        DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) == 0);
      }
    }

    clock_gettime(CLOCK_MONOTONIC, &stop);
    const double elapsed_ns =
        (stop.tv_sec - start.tv_sec) * 1e9 + (stop.tv_nsec - start.tv_nsec);
    // TODO(mcasas): find a standardized way to produce results.
    printf("%f ms\n", elapsed_ns / 1000000.0);

    for (size_t j = 0; j < NUM_BOS; j++) {
      bs_mapper_unmap(mapper, bos[j], map_data_y[j]);
      bs_mapper_unmap(mapper, bos[j], map_data_uv[j]);

      for (size_t plane = 0; plane < NUM_PLANES; plane++)
        close(gbm_bo_fds[plane][j]);
      gbm_bo_destroy(bos[j]);
    }
  }

  free(draft_canvas);

  // Not really needed, but good to destroy things properly.
  bs_mapper_destroy(mapper);
  gbm_device_destroy(gbm);

  return EXIT_SUCCESS;
}
