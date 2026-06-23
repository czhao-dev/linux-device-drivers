/* Standalone ioctl stats dump for /dev/vblk0. */
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../vblk.h"

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/vblk0";
	struct vblk_stats stats;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (ioctl(fd, VBLK_GET_STATS, &stats) != 0) {
		perror("ioctl(VBLK_GET_STATS)");
		close(fd);
		return 1;
	}

	printf("capacity_bytes=%llu sectors=%llu reads=%llu writes=%llu "
	       "read_sectors=%llu write_sectors=%llu\n",
	       (unsigned long long)stats.capacity_bytes,
	       (unsigned long long)stats.sectors,
	       (unsigned long long)stats.reads,
	       (unsigned long long)stats.writes,
	       (unsigned long long)stats.read_sectors,
	       (unsigned long long)stats.write_sectors);

	close(fd);
	return 0;
}
