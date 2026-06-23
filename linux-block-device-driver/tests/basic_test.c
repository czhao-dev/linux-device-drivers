/*
 * Raw block I/O + ioctl roundtrip test for /dev/vblk0.
 *
 * Uses O_DIRECT so every pread/pwrite bypasses the page cache and actually
 * reaches vblk_queue_rq() — without it, a pread() immediately following a
 * pwrite() to the same offset is satisfied straight from the cached page
 * and never reaches the driver, which would make the ioctl read-counter
 * assertions below meaningless. O_DIRECT requires the buffer address,
 * offset, and length to all be aligned to the device's logical block size,
 * hence posix_memalign() instead of stack buffers.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>

#include "../vblk.h"

#define ALIGN 4096

#define CHECK(cond, msg)                                                     \
	do {                                                                  \
		if (!(cond)) {                                                \
			fprintf(stderr, "FAIL: %s\n", msg);                  \
			exit(1);                                              \
		}                                                             \
	} while (0)

static void *aligned_buf(size_t len)
{
	void *p;

	if (posix_memalign(&p, ALIGN, len) != 0) {
		fprintf(stderr, "FAIL: posix_memalign\n");
		exit(1);
	}
	return p;
}

static void fill_pattern(unsigned char *buf, size_t len, unsigned char seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (unsigned char)(seed + i);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/vblk0";
	int fd;
	unsigned char *wbuf = aligned_buf(ALIGN);
	unsigned char *rbuf = aligned_buf(ALIGN);
	struct vblk_stats stats;

	fd = open(path, O_RDWR | O_DIRECT);
	CHECK(fd >= 0, "open");

	/* Roundtrip at offset 0 */
	fill_pattern(wbuf, ALIGN, 0x11);
	CHECK(pwrite(fd, wbuf, ALIGN, 0) == (ssize_t)ALIGN, "pwrite at offset 0");
	CHECK(pread(fd, rbuf, ALIGN, 0) == (ssize_t)ALIGN, "pread at offset 0");
	CHECK(memcmp(wbuf, rbuf, ALIGN) == 0, "roundtrip mismatch at offset 0");

	/* Roundtrip at a non-zero offset (1 MiB) */
	fill_pattern(wbuf, ALIGN, 0x22);
	off_t off = 1 * 1024 * 1024;
	CHECK(pwrite(fd, wbuf, ALIGN, off) == (ssize_t)ALIGN, "pwrite at 1MiB offset");
	CHECK(pread(fd, rbuf, ALIGN, off) == (ssize_t)ALIGN, "pread at 1MiB offset");
	CHECK(memcmp(wbuf, rbuf, ALIGN) == 0, "roundtrip mismatch at 1MiB offset");

	/* Multi-segment write via pwritev (best-effort exercise of multi-bvec requests) */
	{
		unsigned char *seg0 = aligned_buf(ALIGN);
		unsigned char *seg1 = aligned_buf(ALIGN);
		unsigned char *seg2 = aligned_buf(ALIGN);
		unsigned char *seg3 = aligned_buf(ALIGN);
		unsigned char *combined = aligned_buf(4 * ALIGN);
		unsigned char *expect = aligned_buf(4 * ALIGN);
		struct iovec iov[4];
		off_t voff = 2 * 1024 * 1024;

		fill_pattern(seg0, ALIGN, 0x30);
		fill_pattern(seg1, ALIGN, 0x40);
		fill_pattern(seg2, ALIGN, 0x50);
		fill_pattern(seg3, ALIGN, 0x60);

		iov[0].iov_base = seg0; iov[0].iov_len = ALIGN;
		iov[1].iov_base = seg1; iov[1].iov_len = ALIGN;
		iov[2].iov_base = seg2; iov[2].iov_len = ALIGN;
		iov[3].iov_base = seg3; iov[3].iov_len = ALIGN;

		CHECK(pwritev(fd, iov, 4, voff) == (ssize_t)(4 * ALIGN),
		      "pwritev multi-segment write");

		memcpy(expect + 0 * ALIGN, seg0, ALIGN);
		memcpy(expect + 1 * ALIGN, seg1, ALIGN);
		memcpy(expect + 2 * ALIGN, seg2, ALIGN);
		memcpy(expect + 3 * ALIGN, seg3, ALIGN);
		CHECK(pread(fd, combined, 4 * ALIGN, voff) == (ssize_t)(4 * ALIGN),
		      "re-read multi-segment write");
		CHECK(memcmp(combined, expect, 4 * ALIGN) == 0,
		      "multi-segment roundtrip mismatch");

		free(seg0); free(seg1); free(seg2); free(seg3);
		free(combined); free(expect);
	}

	/* ioctl stats sanity check */
	CHECK(ioctl(fd, VBLK_GET_STATS, &stats) == 0, "ioctl VBLK_GET_STATS");
	CHECK(stats.capacity_bytes > 0, "capacity_bytes is zero");
	CHECK(stats.sectors == stats.capacity_bytes / 512, "sectors != capacity_bytes/512");
	CHECK(stats.reads >= 1, "reads counter did not advance");
	CHECK(stats.writes >= 1, "writes counter did not advance");

	printf("capacity_bytes=%llu sectors=%llu reads=%llu writes=%llu\n",
	       (unsigned long long)stats.capacity_bytes,
	       (unsigned long long)stats.sectors,
	       (unsigned long long)stats.reads,
	       (unsigned long long)stats.writes);

	free(wbuf);
	free(rbuf);
	close(fd);
	printf("basic_test: PASS\n");
	return 0;
}
