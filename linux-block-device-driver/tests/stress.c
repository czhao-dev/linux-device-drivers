/*
 * Concurrent stress test for /dev/vblk0.
 *
 * Unlike circbuf's stress test (a shared FIFO where "total written ==
 * total read" is the invariant), a block device is random-access: each
 * thread is given its own disjoint byte region of the disk and only ever
 * reads/writes within it. The invariant under test is therefore "many
 * threads issuing concurrent I/O to the same request_queue never corrupt
 * each other's disjoint regions" — verified by mirroring every write into
 * an in-memory shadow buffer and, after all threads finish, re-reading
 * each region from the device and comparing it against that thread's
 * shadow.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../vblk.h"

#define SECTOR_SIZE 512

struct thread_ctx {
	int id;
	const char *path;
	off_t region_off;
	size_t region_size;
	int duration_s;

	unsigned char *shadow;
	unsigned long long bytes_written;
	int failed;
};

static double now_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *worker(void *arg)
{
	struct thread_ctx *ctx = arg;
	unsigned int seed = (unsigned int)(ctx->id * 7919 + 1);
	double deadline = now_seconds() + ctx->duration_s;
	int fd;
	unsigned char buf[4096], verify[4096];

	fd = open(ctx->path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "thread %d: open failed\n", ctx->id);
		ctx->failed = 1;
		return NULL;
	}

	size_t max_sectors = (ctx->region_size / SECTOR_SIZE);

	while (now_seconds() < deadline) {
		size_t len_sectors = 1 + (rand_r(&seed) % 8); /* 1..8 sectors */
		size_t len = len_sectors * SECTOR_SIZE;

		if (len > ctx->region_size)
			len = ctx->region_size - (ctx->region_size % SECTOR_SIZE);

		size_t max_off_sectors = max_sectors - len_sectors;
		size_t off_sectors = max_off_sectors ? (rand_r(&seed) % (max_off_sectors + 1)) : 0;
		size_t off = off_sectors * SECTOR_SIZE;

		size_t i;
		unsigned char tag = (unsigned char)(ctx->id * 31 + seed);

		for (i = 0; i < len; i++)
			buf[i] = (unsigned char)(tag + i);

		if (pwrite(fd, buf, len, ctx->region_off + off) != (ssize_t)len) {
			fprintf(stderr, "thread %d: pwrite failed at off=%zu len=%zu\n",
				ctx->id, off, len);
			ctx->failed = 1;
			break;
		}
		memcpy(ctx->shadow + off, buf, len);

		if (pread(fd, verify, len, ctx->region_off + off) != (ssize_t)len) {
			fprintf(stderr, "thread %d: pread failed at off=%zu len=%zu\n",
				ctx->id, off, len);
			ctx->failed = 1;
			break;
		}
		if (memcmp(buf, verify, len) != 0) {
			fprintf(stderr, "thread %d: immediate verify mismatch at off=%zu\n",
				ctx->id, off);
			ctx->failed = 1;
			break;
		}

		ctx->bytes_written += len;
	}

	close(fd);
	return NULL;
}

int main(int argc, char **argv)
{
	const char *path = "/dev/vblk0";
	int num_threads = 4;
	int duration_s = 5;
	int i;

	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--writers=", 10) == 0)
			num_threads = atoi(argv[i] + 10);
		else if (strncmp(argv[i], "--duration=", 11) == 0)
			duration_s = atoi(argv[i] + 11);
		else
			path = argv[i];
	}

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	struct vblk_stats stats;
	if (ioctl(fd, VBLK_GET_STATS, &stats) != 0) {
		perror("ioctl(VBLK_GET_STATS)");
		close(fd);
		return 1;
	}
	close(fd);

	size_t region_size = (size_t)(stats.capacity_bytes / num_threads);
	region_size -= region_size % SECTOR_SIZE;

	printf("== stress ==\n");
	printf("device=%s threads=%d duration=%ds region_size=%zu\n",
	       path, num_threads, duration_s, region_size);

	struct thread_ctx *ctxs = calloc(num_threads, sizeof(*ctxs));
	pthread_t *tids = calloc(num_threads, sizeof(*tids));

	for (i = 0; i < num_threads; i++) {
		ctxs[i].id = i;
		ctxs[i].path = path;
		ctxs[i].region_off = (off_t)i * region_size;
		ctxs[i].region_size = region_size;
		ctxs[i].duration_s = duration_s;
		ctxs[i].shadow = calloc(1, region_size);
		pthread_create(&tids[i], NULL, worker, &ctxs[i]);
	}

	int any_failed = 0;
	unsigned long long total_written = 0;

	for (i = 0; i < num_threads; i++) {
		pthread_join(tids[i], NULL);
		total_written += ctxs[i].bytes_written;
		if (ctxs[i].failed)
			any_failed = 1;
	}

	/* Final cross-region corruption check */
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open (verify)");
		return 1;
	}

	unsigned long long total_read = 0;
	unsigned char *readback = malloc(region_size);

	for (i = 0; i < num_threads; i++) {
		if (pread(fd, readback, region_size, ctxs[i].region_off) != (ssize_t)region_size) {
			fprintf(stderr, "final verify: pread failed for region %d\n", i);
			any_failed = 1;
			continue;
		}
		total_read += region_size;
		if (memcmp(readback, ctxs[i].shadow, region_size) != 0) {
			fprintf(stderr, "corruption detected in region %d\n", i);
			any_failed = 1;
		}
	}

	close(fd);
	free(readback);
	for (i = 0; i < num_threads; i++)
		free(ctxs[i].shadow);
	free(ctxs);
	free(tids);

	printf("total_written=%llu total_read=%llu\n", total_written, total_read);
	printf("stress: %s\n", any_failed ? "FAIL" : "PASS");

	return any_failed ? 1 : 0;
}
