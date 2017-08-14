#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sync/sync.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/* These are not exported from the library because they are for testing only. */
int sw_sync_fence_create(int fd, const char *name, unsigned value);
int sw_sync_timeline_create(void);
int sw_sync_timeline_inc(int fd, unsigned count);

#define CHK(errn) do { int ret = fc; if (ret < 0) return ret; } while (0)

#define ERROR(msg, en) do {\
		fprintf(stderr, "ERROR : %s. : %d:%s\n", (msg), (en), strerror(en)); \
	} while (0)

bool verbose = false;

static int
test_swsync_exists(void)
{
	int ret;
	int newsync = 0;
	struct stat buf = { 0 };

	ret = stat("/dev/sw_sync", &buf);
	if (ret < 0 && errno == ENOENT) {
		ret = stat("/sys/kernel/debug/sync/sw_sync", &buf);
		if (ret < 0 && errno == ENOENT) {
			ret = -errno;
			ERROR("sw_sync interface does not exist", -ret);
			goto fail;
		}
		newsync = 1;
	}
	if (ret < 0 && errno == EACCES) {
		ret = -errno;
		ERROR("No permission to access sw_sync interface", -ret);
		goto fail;
	}

	if (ret < 0) {
		ret = -errno;
		ERROR("Stat failed", -ret);
		goto fail;
	}

	/* Old sync is a character device. New is a regular file in sysfs. */
	if ((!newsync && (buf.st_mode & S_IFCHR))
	    || (newsync && (buf.st_mode & S_IFREG))) {
		ret = 0;
	}

fail:
	return ret;
}

static int
get_timeline(void)
{
	int tl = sw_sync_timeline_create();
	if (tl < 0) {
		tl = -errno;
		ERROR("Cannot create timeline", -tl);
	}
	return tl;
}

static int
get_fence(int tl, int value)
{
	int fc = sw_sync_fence_create(tl, "simple fence", value);
	if (fc < 0) {
		fc = -errno;
		ERROR("Failed to create fence", -fc);
	}
	return fc;
}

static int
fence_check(int fc)
{
	int ret;
	ret = sync_wait(fc, 0);
	if (ret < 0) {
		if (errno == ETIME)
			return 0;
		if (errno == EINVAL)
			return 1; /* timeline destroyed */
		ret = -errno;
		ERROR("Failed fence wait", -ret);
		return ret;
	}

	return 1;
}

/* Return 0 when timeout, 1 when passing, < 0 for error
*/
static int
timeline_inc(int tl, int delta)
{
	int ret;
	ret = sw_sync_timeline_inc(tl, delta);
	if (ret < 0) {
		ret = -errno;
		ERROR("Failed to increment timeline", -ret);
	}
	return ret;
}

/* Test single fence. */
static int
test_simple(void)
{
	int tl;
	int fc;
	int ret;

	tl = get_timeline();
	if (tl < 0) {
		return tl;
	}

	fc = get_fence(tl, 1);
	if (fc < 0) {
		ret = fc;
		goto fail;
	}

	ret = fence_check(fc);
	if (ret < 0) {
		goto fail_fence;
	} else if (ret == 1) {
		ret = -1;
		ERROR("Fence already signaled even though we did not signal it", 0);
		goto fail_fence;
	}

	ret = timeline_inc(tl, 1);
	if (ret < 0) {
		goto fail_fence;
	}

	ret = fence_check(fc);
	if (ret < 0) {
		goto fail_fence;
	} else if (ret == 0) {
		ret = -1;
		ERROR("Fence is not signaled even though it should be", 0);
		goto fail_fence;
	}
	ret = 0;

fail_fence:
	close(fc);
fail:
	close(tl);
	return ret;
}


#define NUM_FENCES 10
/* Test multiple fences signaled with one timeline inc. */
static int
test_multiple(void)
{
	int i;
	int tl;
	int fc[NUM_FENCES];
	int ret;
	int value = 1;

	tl = get_timeline();
	if (tl < 0) {
		return tl;
	}

	for (i = 0; i < NUM_FENCES; i++) {
		fc[i] = get_fence(tl, value++);
		if (fc[i] < 0) {
			ret = fc[i];
			goto fail;
		}
	}

	for (i = 0; i < NUM_FENCES; i++) {
		ret = fence_check(fc[i]);
		if (ret < 0) {
			goto fail_fence;
		} else if (ret == 1) {
			ret = -1;
			ERROR("Fence already signaled even though we did not signal it", 0);
			goto fail_fence;
		}
	}

	ret = timeline_inc(tl, NUM_FENCES);
	if (ret < 0) {
		goto fail_fence;
	}

	for (i = 0; i < NUM_FENCES; i++) {
		ret = fence_check(fc[i]);
		if (ret < 0) {
			goto fail_fence;
		} else if (ret == 0) {
			ret = -1;
			ERROR("Fence is not signaled even though it should be", 0);
			goto fail_fence;
		}
	}
	ret = 0;

fail_fence:
	for (i = 0; i < NUM_FENCES; i++)
		close(fc[i]);
fail:
	close(tl);
	return ret;
}
#undef NUM_FENCES

/* Test destruction of timeline. It should signal/set error condition on all
 * fences in the timeline. */
static int
test_timeline_destroy(void)
{
	int tl;
	int fc;
	int ret;

	tl = get_timeline();
	if (tl < 0) {
		return tl;
	}

	fc = get_fence(tl, 1);
	if (fc < 0) {
		ret = fc;
		goto fail;
	}

	ret = fence_check(fc);
	if (ret < 0) {
		goto fail_fence;
	} else if (ret == 1) {
		ret = -1;
		ERROR("Fence already signaled even though we did not signal it", 0);
		goto fail_fence;
	}

	close(tl);

	ret = fence_check(fc);
	if (ret < 0) {
		goto fail_fence;
	} else if (ret == 0) {
		ret = -1;
		ERROR("Fence is not signaled even though it should be", 0);
		goto fail_fence;
	}
	ret = 0;

fail_fence:
	close(fc);
fail:
	return ret;
}

static const struct { const char *name; int (*func)(void); } cases[] = {
	{ "swsync_exists", test_swsync_exists },
	{ "simple", test_simple },
	{ "multiple", test_multiple },
	{ "timeline_destroy", test_timeline_destroy },
	{ NULL, NULL }
};


static const struct option longopts[] = {
	{ "test_name", required_argument, NULL, 't' },
	{ "verbose", no_argument, NULL, 'v' },
	{ "help", no_argument, NULL, 'h' },
	{ 0, 0, 0, 0 },
};

static void print_help(const char *argv0)
{
	unsigned int u;
	printf("usage: %s -t <test_name> [-v]\n", argv0);
	printf("A valid name test is one the following:\n");
	for (u = 0; cases[u].name; u++)
		printf("%s\n", cases[u].name);
	printf("all\n");
}

int
run_synctest(const char *name)
{
	unsigned int u;
	int ret = 0;

	for (u = 0; cases[u].name; u++) {
		if (strcmp(cases[u].name, name) && strcmp(name, "all"))
			continue;
		if (verbose)
			printf("Running subtest %s\n", cases[u].name);
		ret = cases[u].func();
		if (verbose)
			printf("Subtest %s %s\n", cases[u].name, (ret < 0) ? "failed" : "passed");
		if (ret < 0)
			break;
	}

	return ret;
}

int
main(int ARGC, char *ARGV[])
{
	int c;
	char *name = strdup("all");

	while ((c = getopt_long(ARGC, ARGV, "t:vh", longopts, NULL)) != -1) {
		switch (c) {
			case 't':
				if (name) {
					free(name);
					name = NULL;
				}

				name = strdup(optarg);
				break;
			case 'v':
				verbose = true;
				break;
			case 'h':
				goto print;
			default:
				goto print;
		}
	}

	if (!name)
		goto print;

	int ret = run_synctest(name);
	if (ret == 0)
		printf("[  PASSED  ] synctest.%s\n", name);
	else if (ret < 0)
		printf("[  FAILED  ] synctest.%s\n", name);

	free(name);

	if (ret > 0) {
		printf("Unexpected test result.\n");
		goto print;
	}

	return ret;

print:
	print_help(ARGV[0]);
	return -EINVAL;

}
