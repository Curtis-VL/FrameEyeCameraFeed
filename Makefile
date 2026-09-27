# Local build.  Release binaries are built by .github/workflows/release.yml;
# `make static` is the same fully static build it does (needs static libjpeg,
# e.g. Alpine's libjpeg-turbo-static).

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
LDLIBS_STREAM = -ljpeg -lpthread -lm

all: framestream framecap

framestream: framestream.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS_STREAM)

framecap: framecap.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

static:
	$(MAKE) LDFLAGS="-static -s" all

clean:
	rm -f framestream framecap

.PHONY: all static clean
