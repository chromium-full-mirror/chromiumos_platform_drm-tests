/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * This is a test meant to exercise the VGEM DRM kernel module's PRIME
 * import/export functions. It will create a gem buffer object, mmap, write, and
 * then verify it. Then the test will repeat that with the same gem buffer, but
 * exported and then imported. Finally, a new gem buffer object is made in a
 * different driver which exports into VGEM and the mmap, write, verify sequence
 * is repeated on that.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <xf86drm.h>
#include <linux/udmabuf.h>

#include "bs_drm.h"

#define HANDLE_EINTR_AND_EAGAIN(x)					       \
	({								       \
		int result;						       \
		do {							       \
			result = (x);					       \
		} while (result != -1 && (errno == EINTR || errno == EAGAIN)); \
		result;							       \
	})

#define fail_if(cond, ...)						\
	do {								\
		if (cond) {						\
			bs_debug_print("FAIL", __func__, __FILE__, __LINE__, __VA_ARGS__); \
			exit(EXIT_FAILURE);				\
		}							\
	} while (0)

const uint32_t g_bo_pattern = 0xdeadbeef;

void *mmap_dumb_bo(int fd, int handle, size_t size)
{
	struct drm_mode_map_dumb mmap_arg;
	void *ptr;
	int ret;

	memset(&mmap_arg, 0, sizeof(mmap_arg));

	mmap_arg.handle = handle;

	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mmap_arg);
	assert(ret == 0);
	assert(mmap_arg.offset != 0);

	ptr = mmap(NULL, size, (PROT_READ | PROT_WRITE), MAP_SHARED, fd, mmap_arg.offset);

	return ptr;
}

void write_pattern(uint32_t *bo_ptr, size_t bo_size)
{
	uint32_t *ptr;

	for (ptr = bo_ptr; ptr < bo_ptr + (bo_size / sizeof(*bo_ptr)); ptr++) {
		*ptr = g_bo_pattern;
	}
}

bool verify_pattern(uint32_t *bo_ptr, size_t bo_size)
{
	uint32_t *ptr;

	for (ptr = bo_ptr; ptr < bo_ptr + (bo_size / sizeof(*bo_ptr)); ptr++) {
		fail_if(*ptr != g_bo_pattern, "buffer object verify");
	}

	return true;
}

int create_udmabuf(int fd, size_t length)
{
	int udmabuf_dev_fd = HANDLE_EINTR_AND_EAGAIN(open("/dev/udmabuf", O_RDWR));
	fail_if(udmabuf_dev_fd < 0, "error opening /dev/udmabuf");

	struct udmabuf_create create;
	create.memfd = fd;
	create.flags = UDMABUF_FLAGS_CLOEXEC;
	create.offset = 0;
	create.size = length;

	int dmabuf_fd = HANDLE_EINTR_AND_EAGAIN(ioctl(udmabuf_dev_fd, UDMABUF_CREATE, &create));
	fail_if(dmabuf_fd < 0, "error creating udmabuf");

	close(udmabuf_dev_fd);
	return dmabuf_fd;
}

int create_memfd(size_t length)
{
	int fd = memfd_create("test memfd", MFD_ALLOW_SEALING);
	fail_if(fd == -1, "memfd_create() error: %s", strerror(errno));

	int res = HANDLE_EINTR_AND_EAGAIN(ftruncate(fd, length));
	fail_if(res == -1, "ftruncate() error: %s", strerror(errno));

	// udmabuf_create requires that file descriptors be sealed with
	// F_SEAL_SHRINK.
	fail_if(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) < 0,
		"fcntl() error: %s", strerror(errno));

	return fd;
}

void test_import_dma_buf(int native_fd)
{
	int width = 256;
	int height = 256;
	int bpp = 4;
	int stride = width * bpp;
	size_t size = stride * height;

	int memfd_fd = create_memfd(size);
	fail_if(memfd_fd == -1, "failed to create memfd");

	uint32_t *memfd_map =
	    mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_SHARED, memfd_fd, 0);
	fail_if(memfd_map == MAP_FAILED, "failed to mmap memfd");

	write_pattern(memfd_map, size);
	fail_if(!verify_pattern(memfd_map, size), "failed to verify pattern");

	fail_if(munmap(memfd_map, size) != 0, "munmap failed");

	int udmabuf_fd = create_udmabuf(memfd_fd, size);
	fail_if(udmabuf_fd == -1, "failed to create udmabuf");

	fail_if(close(memfd_fd) != 0, "close memfd failed");

	uint32_t foreign_imported_handle;
	fail_if(drmPrimeFDToHandle(native_fd, udmabuf_fd, &foreign_imported_handle) != 0,
		"drmPrimeFDToHandle failed");

	uint32_t *bo_ptr =
		mmap_dumb_bo(native_fd, foreign_imported_handle, size);
	fail_if(bo_ptr == MAP_FAILED,
		"failed to map imported buffer object\n");

	fail_if(close(udmabuf_fd) != 0, "failed to close udmabuf_fd");

	fail_if(!verify_pattern(bo_ptr, size), "pattern mismatch after map");
}

void test_export_dma_buf(int native_fd)
{
	struct drm_mode_create_dumb create;
	uint32_t *bo_ptr;

	memset(&create, 0, sizeof(create));
	create.width = 640;
	create.height = 480;
	create.bpp = 32;

	fail_if(drmIoctl(native_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0,
		"failed to create dumb buffer object");

	bo_ptr = mmap_dumb_bo(native_fd, create.handle, create.size);
	fail_if(bo_ptr == MAP_FAILED, "failed to map dumb bo");

	write_pattern(bo_ptr, create.size);

	munmap(bo_ptr, create.size);

	int dmabuf_fd;
	fail_if(drmPrimeHandleToFD(native_fd, create.handle, DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd) != 0,
		"drmPrimeHandleToFD failed");

	uint32_t *dmabuf_map =
	    mmap(NULL, create.size, PROT_WRITE | PROT_READ, MAP_SHARED, dmabuf_fd, 0);
	fail_if(dmabuf_map == MAP_FAILED, "failed to mmap memfd");

	fail_if(!verify_pattern(dmabuf_map, create.size),
		"pattern mismatch after gbm_bo_map");

	fail_if(munmap(dmabuf_map, create.size) != 0, "munmap failed");

	fail_if(close(dmabuf_fd) != 0, "failed to close dmabuf_fd");
}

int main(int argc, char *argv[])
{
	int fd = bs_drm_open_for_display();
	fail_if(fd == -1, "error opening dri card");

	test_import_dma_buf(fd);

	test_export_dma_buf(fd);

	close(fd);

	return EXIT_SUCCESS;
}
