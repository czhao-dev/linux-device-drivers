CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -I..
# Static binaries so they can run from a minimal busybox initramfs (no glibc on target)
LDFLAGS ?= -static

BINARIES = basic_test query_stats stress

all: $(BINARIES)

basic_test: basic_test.c ../vblk.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

query_stats: query_stats.c ../vblk.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

stress: stress.c ../vblk.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lpthread

clean:
	rm -f $(BINARIES)

.PHONY: all clean
