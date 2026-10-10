CC = gcc
CFLAGS = -I../libane -I../ane/src/uapi/drm -Wall -Werror -Wextra \
	-Wdeclaration-after-statement -O3 -std=gnu99

LIB = ../libane/libane.a

# ane_stats is built with its own flags because it links against the
# host shim in ane_stats.h and uses C11 threads (pthread).
ANE_STATS_CFLAGS = -I.. -Wall -Werror -Wextra -O2 -std=gnu11 -pthread

.PHONY: tools check clean

tools: ane-run ane-session ane-selfcheck

$(LIB):
	$(MAKE) -C ../libane libane

ane-run: ane-run.c ane_f16_add.h $(LIB) ../libane/ane.h ../libane/ane_m2.h
	$(CC) $(CFLAGS) -o $@ ane-run.c $(LIB) -lm

# Resident session (qwen_m2_decode --resident): includes ane-run.c for the
# strict port-table reader, so it rebuilds whenever ane-run.c changes.
ane-session: ane-session.c ane-run.c ane_f16_add.h $(LIB) ../libane/ane.h \
	../libane/ane_m2.h
	$(CC) $(CFLAGS) -o $@ ane-session.c $(LIB) -lm

ane-selfcheck: ane-selfcheck.c ane_f16_add.h $(LIB) ../libane/ane.h \
	../libane/ane_m2.h
	$(CC) $(CFLAGS) -o $@ ane-selfcheck.c $(LIB) -lm

# libane sources rebuilt with every unset stack variable filled with 0xfe,
# and linked against the fake DRM node in test_libane_ioctl.c.
LIBANE_SRC = ../libane/ane.c ../libane/ane_m2.c ../libane/ane_sha256.c

test_libane_ioctl: test_libane_ioctl.c $(LIBANE_SRC) ../libane/ane.h \
	../libane/ane_m2.h ../libane/ane_bind.h ../libane/ane_f16.h \
	../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -I/usr/include/libdrm -ftrivial-auto-var-init=pattern \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap \
		-o $@ test_libane_ioctl.c $(LIBANE_SRC) -lm

# ane_stats host unit test: pure host compile of the shared header
# (counters + ring + torn-read + union rule + stats=0 path) plus the
# REAL ane/ane_stats_show.c against the stub kernel headers in
# ane_stats_shim/ (emit output, timeline render, ticket/seq protocol).
test_ane_stats: test_ane_stats.c ../ane/include/ane_stats.h \
		../ane/ane_stats_show.c ane_stats_shim/linux/fs.h \
		ane_stats_shim/linux/module.h \
		ane_stats_shim/linux/seq_file.h
	$(CC) $(ANE_STATS_CFLAGS) -Iane_stats_shim -I../ane/include \
		-Wno-unused-parameter -o $@ \
		test_ane_stats.c ../ane/ane_stats_show.c


ane-bo-probe: ane-bo-probe.c ../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -I../ane/src/uapi/drm -I/usr/include/libdrm -o $@ ane-bo-probe.c

# Pool policy unit test: compiles the driver's ane_t6021_pool.h against
# the stub kernel headers in ane_pool_shim/linux (ane_stats_shim pattern).
test_t6021_pool: test_t6021_pool.c ../ane/t6021/ane_t6021_pool.h \
	ane_pool_shim/linux/list.h ane_pool_shim/linux/types.h \
	ane_pool_shim/linux/spinlock.h ane_pool_shim/linux/atomic.h \
	ane_pool_shim/linux/mm.h
	$(CC) $(CFLAGS) -Iane_pool_shim -o $@ test_t6021_pool.c

# ane-session LOAD cache: drives the real session main over the fake
# DRM node and counts the BO_INITs a repeated LOAD issues.
test_ane_session_cache: test_ane_session_cache.c ane-session.c ane-run.c \
	$(LIBANE_SRC) ../libane/ane.h ../libane/ane_m2.h ../libane/ane_bind.h \
	../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -I../libane -I../ane/src/uapi/drm -I/usr/include/libdrm \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap \
		-o $@ test_ane_session_cache.c $(LIBANE_SRC) -lm

# ane-bo-probe net check: parked vs leaked, over a fake sysfs.
test_ane_bo_probe: test_ane_bo_probe.c ane-bo-probe.c \
	../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -DANE_BO_PROBE_TEST -I../ane/src/uapi/drm \
		-I/usr/include/libdrm \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap \
		-o $@ test_ane_bo_probe.c -lm

# PROG_LOOKUP digest tests: NIST SHA-256 vectors and the driver's
# program-digest construction over a real fixture (host only).
test_t6021_prog_lookup: test_t6021_prog_lookup.c ../libane/ane_sha256.c \
	../libane/ane_sha256.h ../libane/ane_m2.h ../libane/ane.h \
	../libane/ane_bind.h ../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -I../libane -I../ane/src/uapi/drm \
		-I/usr/include/libdrm -o $@ \
		test_t6021_prog_lookup.c ../libane/ane_sha256.c \
		../libane/ane_m2.c ../libane/ane.c -lm

# The same session-cache test under LeakSanitizer: the cache-hit path once
# leaked one port table per hit and no plain run could see it. Skipped when
# the compiler has no AddressSanitizer.
ASAN_OK := $(shell echo 'int main(void){return 0;}' | $(CC) -fsanitize=address -x c - -o /dev/null 2>/dev/null && echo yes)
test_ane_session_cache_lsan: test_ane_session_cache.c ane-session.c ane-run.c \
	$(LIBANE_SRC) ../libane/ane.h ../libane/ane_m2.h ../libane/ane_bind.h \
	../ane/src/uapi/drm/ane_accel.h
	$(CC) -I../libane -I../ane/src/uapi/drm -I/usr/include/libdrm -std=gnu99 -O1 -g \
		-fsanitize=address -fno-omit-frame-pointer \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap \
		-o $@ test_ane_session_cache.c $(LIBANE_SRC) -lm

check: ane-selfcheck test_libane_ioctl test_ane_stats test_t6021_pool \
	test_ane_session_cache test_ane_bo_probe test_t6021_prog_lookup
	./ane-selfcheck ../fixtures/h14-anec
	./test_libane_ioctl ../fixtures/h14-anec
	./test_ane_stats
	./test_t6021_pool
	./test_ane_session_cache
	./test_ane_bo_probe
	./test_t6021_prog_lookup
ifeq ($(ASAN_OK),yes)
	$(MAKE) test_ane_session_cache_lsan
	ASAN_OPTIONS=detect_leaks=1 ./test_ane_session_cache_lsan
endif

# aarch64 cross-check for the SHA-2 crypto block in ane_sha256.c:
# compile -Werror for aarch64 and run the digest suite under
# qemu-aarch64-static (-cpu max enables the extension). Runs in `make
# check` only when both tools exist; silent skip otherwise.
CROSS_CC := $(shell command -v aarch64-linux-gnu-gcc 2>/dev/null)
CROSS_QEMU := $(shell command -v qemu-aarch64-static 2>/dev/null)

cross-check:
ifneq ($(CROSS_CC),)
ifneq ($(CROSS_QEMU),)
	$(CROSS_CC) -std=gnu11 -Wall -Wextra -Werror -O2 \
		-I../libane -I../ane/src/uapi/drm \
		-I/usr/aarch64-linux-gnu/include \
		-I/usr/aarch64-linux-gnu/include/drm \
		-o test_t6021_prog_lookup_a64 test_t6021_prog_lookup.c \
		../libane/ane_sha256.c ../libane/ane_m2.c ../libane/ane.c -lm
	(cd ../fixtures && qemu-aarch64-static -cpu max \
		-L /usr/aarch64-linux-gnu \
		$(abspath test_t6021_prog_lookup_a64) 2>&1) | tee test_sha256_a64.log
	grep -q "LOOKUP-DIGEST PASS" test_sha256_a64.log
	grep -q "path disagreement" test_sha256_a64.log && exit 1 || true
	@echo "cross-check PASS (aarch64 qemu: both paths agree)"
else
	@echo "cross-check SKIP: qemu-aarch64-static not installed"
endif
else
	@echo "cross-check SKIP: aarch64-linux-gnu-gcc not installed"
endif

clean:
	rm -f ane-run ane-selfcheck test_libane_ioctl test_ane_stats test_t6021_pool test_ane_session_cache test_ane_bo_probe test_t6021_prog_lookup test_t6021_prog_lookup_a64 test_sha256_a64.log