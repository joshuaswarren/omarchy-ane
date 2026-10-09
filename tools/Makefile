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
LIBANE_SRC = ../libane/ane.c ../libane/ane_m2.c

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

check: ane-selfcheck test_libane_ioctl test_ane_stats test_t6021_pool \
	test_ane_session_cache test_ane_bo_probe
	./ane-selfcheck ../fixtures/h14-anec
	./test_libane_ioctl ../fixtures/h14-anec
	./test_ane_stats
	./test_t6021_pool

clean:
	rm -f ane-run ane-selfcheck test_libane_ioctl test_ane_stats