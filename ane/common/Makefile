# SPDX-License-Identifier: GPL-2.0
# DART t8110 page-table builder: userspace check only. Not part of
# "make all", not a kernel module, no kernel tree needed.
#
#   make check        build and run the positive/negative harness
#   make clean
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

check: dart_t8110_test
	./dart_t8110_test

dart_t8110_test: dart_t8110_test.c dart_t8110.h
	$(CC) $(CFLAGS) -I. dart_t8110_test.c -o $@

clean:
	rm -f dart_t8110_test

.PHONY: check clean
