# test/host/Makefile — native (host) unit tests.
#
# Parts of the kernel are pure logic over plain memory with no hardware
# dependency and can therefore be compiled and run on the build machine,
# where they are fast, debuggable, and don't need a USB-stick round trip.
#
#     make -C test/host run     # build and run everything, non-zero on failure
#
# Include order matters: this directory comes FIRST so critical.h resolves
# to the host stub here instead of the AArch64 inline-asm version in
# ../../include.  Everything else resolves to the real kernel headers, so
# the tests exercise the same definitions the kernel is built with.

CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wshadow -fno-strict-aliasing
INCS     = -I. -I../../include

TESTS    = memtest

.PHONY: all run clean
all: $(TESTS)

memtest: memtest.c ../../mem/memory.c ../../include/memory.h critical.h
	$(CC) $(CFLAGS) $(INCS) -o $@ memtest.c

run: $(TESTS)
	@rc=0; \
	for t in $(TESTS); do \
	    echo "--- $$t ---"; \
	    ./$$t || rc=1; \
	done; \
	if [ $$rc -eq 0 ]; then echo "ALL HOST TESTS PASSED"; else echo "HOST TESTS FAILED"; fi; \
	exit $$rc

clean:
	rm -f $(TESTS) *.o
