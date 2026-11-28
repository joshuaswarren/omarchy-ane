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

.PHONY: check-h15-firmware
check-h15-firmware:
	test -n "$(H15_FW_INPUT_DIR)"
	test -n "$(H15_FW_ANE1_INPUT_DIR)"
	H15_FW_INPUT_DIR="$(H15_FW_INPUT_DIR)" H15_FW_ANE1_INPUT_DIR="$(H15_FW_ANE1_INPUT_DIR)" python3 test_h15_firmware.py -v

ane-run: ane-run.c ane_f16_add.h ane_check_band.h ane_timing.h ane_cpu_floor.h $(LIB) ../libane/ane.h ../libane/ane_m2.h
	$(CC) $(CFLAGS) -o $@ ane-run.c $(LIB) -lm

# Resident session (qwen_m2_decode --resident): includes ane-run.c for the
# strict port-table reader, so it rebuilds whenever ane-run.c changes.
ane-session: ane-session.c ane-run.c ane_f16_add.h ane_check_band.h ane_timing.h ane_cpu_floor.h $(LIB) ../libane/ane.h \
	../libane/ane_m2.h
	$(CC) $(CFLAGS) -o $@ ane-session.c $(LIB) -lm

ane-selfcheck: ane-selfcheck.c ane_f16_add.h $(LIB) ../libane/ane.h \
	../libane/ane_m2.h
	$(CC) $(CFLAGS) -o $@ ane-selfcheck.c $(LIB) -lm

# Lane acceptance of ane-run --check matvec and rms, host only (no device).
test_check_band: test_check_band.c ane_check_band.h
	$(CC) $(CFLAGS) -o $@ test_check_band.c -lm

# Mean and raw per-call output of ane-run --time-raw, host only (no device).
test_ane_timing: test_ane_timing.c ane_timing.h
	$(CC) $(CFLAGS) -o $@ test_ane_timing.c -lm

# CPU clock floor (ane_cpu_floor.h): state machine over fake sysfs, host only.
# The test overrides live behind -DANE_CPU_FLOOR_TESTING: the production
# ane-run is built without it and ignores ANE_RUN_CPUFREQ_ROOT and
# ANE_RUN_STATE_DIR.
test_cpu_floor: test_cpu_floor.c ane_cpu_floor.h
	$(CC) $(CFLAGS) -DANE_CPU_FLOOR_TESTING -o $@ test_cpu_floor.c

# The serving stand-in the process-level floor tests signal and kill.
test_cpu_floor_serve: test_cpu_floor_serve.c ane_cpu_floor.h
	$(CC) $(CFLAGS) -DANE_CPU_FLOOR_TESTING -o $@ test_cpu_floor_serve.c

# libane sources rebuilt with every unset stack variable filled with 0xfe,
# and linked against the fake DRM node in test_libane_ioctl.c.
LIBANE_SRC = ../libane/ane.c ../libane/ane_m2.c ../libane/ane_v3.c ../libane/ane_sha256.c

test_libane_ioctl: test_libane_ioctl.c $(LIBANE_SRC) ../libane/ane.h \
	../libane/ane_m2.h ../libane/ane_v3.h ../libane/ane_bind.h ../libane/ane_f16.h \
	../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -I/usr/include/libdrm -ftrivial-auto-var-init=pattern \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap \
		-o $@ test_libane_ioctl.c $(LIBANE_SRC) -lm

# T6050 (M5 Max) and T8122 (base M3) enumeration through the ABI-3 path, host-only.
test_libane_t6050: test_libane_t6050.c $(LIBANE_SRC) ../libane/ane.h \
	../libane/ane_v3.h ../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -I/usr/include/libdrm \
		-Wl,--wrap=open,--wrap=ioctl \
		-o $@ test_libane_t6050.c $(LIBANE_SRC) -lm

# ABI-3 (in-tree H15/H16/H17) section builder, H16_LOAD, mmap in-place and copy-mode EXEC, host-only.
test_libane_v3: test_libane_v3.c $(LIBANE_SRC) ../libane/ane.h \
	../libane/ane_v3.h ../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -I/usr/include/libdrm -ftrivial-auto-var-init=pattern \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap,--wrap=munmap \
		-o $@ test_libane_v3.c $(LIBANE_SRC) -lm

test_libane_h18_resident: test_libane_h18_resident.c ../libane/ane_h18_resident.c ../libane/ane_h18_resident.h ../ane/h18/ane_h18_res_uapi.h
	$(CC) $(CFLAGS) -Wl,--wrap=open,--wrap=write,--wrap=read,--wrap=ioctl -o $@ $(filter %.c,$^)

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


# CPU latency hold (libane/ane_latency.h), host only (no device): a plain file stands in for /dev/cpu_dma_latency.
test_ane_latency: test_ane_latency.c ../libane/ane_latency.h
	$(CC) $(CFLAGS) -o $@ test_ane_latency.c -pthread


ane-bo-probe: ane-bo-probe.c ../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -I../ane/src/uapi/drm -I/usr/include/libdrm -o $@ ane-bo-probe.c

# Pool policy unit test: compiles the driver's ane_t6021_pool.h against
# the stub kernel headers in ane_pool_shim/linux (ane_stats_shim pattern).
test_t6021_pool: test_t6021_pool.c ../ane/t6021/ane_t6021_pool.h \
	ane_pool_shim/linux/list.h ane_pool_shim/linux/types.h \
	ane_pool_shim/linux/spinlock.h ane_pool_shim/linux/atomic.h \
	ane_pool_shim/linux/mm.h
	$(CC) $(CFLAGS) -Iane_pool_shim -o $@ test_t6021_pool.c

# Program table and unload rule: compiles the driver's ane_t6021_progtab.h against the same stub kernel headers.
test_t6021_progtab: test_t6021_progtab.c ../ane/t6021/ane_t6021_progtab.h \
	ane_pool_shim/linux/types.h ane_pool_shim/linux/string.h
	$(CC) $(CFLAGS) -Iane_pool_shim -o $@ test_t6021_progtab.c

# Shared-memory ring entries: compiles the driver's ane_t6021_mring.h against the same stub kernel headers.
test_t6021_mring: test_t6021_mring.c ../ane/t6021/ane_t6021_mring.h ane_pool_shim/linux/types.h
	$(CC) $(CFLAGS) -Iane_pool_shim -o $@ test_t6021_mring.c

# Legacy buffer table: compiles the driver's ane_t6021_legtab.h against the same stub kernel headers.
test_t6021_legtab: test_t6021_legtab.c ../ane/t6021/ane_t6021_legtab.h ane_pool_shim/linux/types.h
	$(CC) $(CFLAGS) -Iane_pool_shim -o $@ test_t6021_legtab.c

# ane-session LOAD cache: drives the real session main over the fake
# DRM node and counts the BO_INITs a repeated LOAD issues.
test_ane_session_cache: test_ane_session_cache.c ane-session.c ane-run.c ane_cpu_floor.h \
	$(LIBANE_SRC) ../libane/ane.h ../libane/ane_m2.h ../libane/ane_bind.h \
	../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -I../libane -I../ane/src/uapi/drm -I/usr/include/libdrm \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap \
		-o $@ test_ane_session_cache.c $(LIBANE_SRC) -lm

# ane-session STATE and RESET (resident states): the real session over the fake DRM node with a
# fake device that writes nonzero padding; no argument runs the checks, any argument serves the
# session protocol for test_qwen_resident_states.py. Mutation proof: test_ane_session_state_mutants.py.
test_ane_session_state: test_ane_session_state.c ane-session.c ane-run.c ane_cpu_floor.h \
	$(LIBANE_SRC) ../libane/ane.h ../libane/ane_m2.h ../libane/ane_bind.h \
	../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -Wno-error=maybe-uninitialized -I../libane -I../ane/src/uapi/drm -I/usr/include/libdrm \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap \
		-o $@ test_ane_session_state.c $(LIBANE_SRC) -lm

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
	../libane/ane_sha256.h ../libane/ane_m2.h ../libane/ane_v3.h ../libane/ane.h \
	../libane/ane_bind.h ../ane/src/uapi/drm/ane_accel.h
	$(CC) $(CFLAGS) -I../libane -I../ane/src/uapi/drm \
		-I/usr/include/libdrm -o $@ \
		test_t6021_prog_lookup.c ../libane/ane_sha256.c \
		../libane/ane_m2.c ../libane/ane_v3.c ../libane/ane.c -lm

# The same session-cache test under LeakSanitizer: the cache-hit path once
# leaked one port table per hit and no plain run could see it. Skipped when
# the compiler has no AddressSanitizer.
# The resident Python tests import numpy. Run them only when it is installed (the overlays CI job installs none).
PYTHON ?= python3
NUMPY_OK := $(shell $(PYTHON) -c 'import numpy' >/dev/null 2>&1 && echo yes)

ASAN_OK := $(shell echo 'int main(void){return 0;}' | $(CC) -fsanitize=address -x c - -o /dev/null 2>/dev/null && echo yes)
# ASan dies at startup (exit 139) under ASLR when vm.mmap_rnd_bits > 28 (README.md); empty where setarch is missing.
NOASLR := $(shell setarch $$(uname -m) -R true 2>/dev/null && echo setarch $$(uname -m) -R)
test_ane_session_cache_lsan: test_ane_session_cache.c ane-session.c ane-run.c ane_cpu_floor.h \
	$(LIBANE_SRC) ../libane/ane.h ../libane/ane_m2.h ../libane/ane_bind.h \
	../ane/src/uapi/drm/ane_accel.h
	$(CC) -I../libane -I../ane/src/uapi/drm -I/usr/include/libdrm -std=gnu99 -O1 -g \
		-fsanitize=address -fno-omit-frame-pointer \
		-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap \
		-o $@ test_ane_session_cache.c $(LIBANE_SRC) -lm

check: ane-run ane-selfcheck ane-session test_libane_ioctl test_libane_t6050 test_libane_v3 test_ane_stats test_t6021_pool test_t6021_progtab test_t6021_mring test_t6021_legtab \
	test_ane_session_cache test_ane_session_state test_ane_bo_probe test_t6021_prog_lookup test_check_band test_ane_timing test_ane_latency \
	test_cpu_floor test_cpu_floor_serve test_libane_h18_resident
	sh test_ane_run_tile_shift.sh ./ane-run
	./test_libane_t6050
	./test_libane_v3 ../fixtures/h15-hwx/add/model.hwx ../fixtures/h15-hwx/add/load/base
	$(PYTHON) test_j615_kit.py
	./test_libane_h18_resident
	sh test_port_reader.sh
	./ane-selfcheck ../fixtures/h14-anec
	./test_libane_ioctl ../fixtures/h14-anec
	sh test_private_tmpdir.sh ./test_libane_ioctl ../fixtures/h14-anec
	./test_ane_stats
	./test_t6021_pool
	./test_ane_session_cache
	./test_ane_session_state
	$(PYTHON) test_ane_session_lock.py
	./test_ane_bo_probe
	./test_t6021_prog_lookup
	./test_check_band
	./test_ane_timing
	./test_ane_latency
	./test_ane_latency off
	./test_ane_latency missing
	./test_ane_latency default
	./test_ane_latency explicit
	./test_ane_latency clamp
	$(PYTHON) ../ane/test/retirement_model_test.py
	$(PYTHON) ../ane/test/tm_timeout_warn_test.py
	$(PYTHON) ../ane/test/tm_transient_fault_test.py
	$(PYTHON) ../ane/test/tm_close_test.py
	$(PYTHON) ../ane/test/tm_recovery_guard_test.py
	$(PYTHON) ../ane/test/tm_telemetry_test.py
	$(PYTHON) test_ane_run_expect_bind.py
	$(PYTHON) test_h15_load.py
	./test_cpu_floor
	$(PYTHON) test_cpu_floor.py
	$(PYTHON) test_cpu_floor_mutants.py
ifeq ($(NUMPY_OK),yes)
	$(PYTHON) test_qwen_resident.py
	$(PYTHON) test_qwen_session_lock_fd.py
	$(PYTHON) test_qwen_resident_ab.py
	$(PYTHON) test_qwen_resident_proxy.py
	$(PYTHON) test_qwen_resident_states.py
	$(PYTHON) test_qwen_resident_states_ab.py
	$(PYTHON) test_qwen_placement.py
	$(PYTHON) test_qwen_q6k_hook.py
	$(PYTHON) test_qwen_q6k_gate.py
	$(PYTHON) test_matvec_golden.py
else
	@echo "SKIP resident Python tests: numpy is not installed"
endif
ifeq ($(ASAN_OK),yes)
	$(MAKE) test_ane_session_cache_lsan
	ASAN_OPTIONS=detect_leaks=1 $(NOASLR) ./test_ane_session_cache_lsan
endif

# aarch64 cross-check for the SHA-2 crypto block in ane_sha256.c:
# compile -Werror for aarch64 and run the digest suite under
# qemu-aarch64-static (-cpu max enables the extension). Invoke explicitly with
# `make -C tools cross-check`; prints SKIP when either tool is missing.
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
		../libane/ane_sha256.c ../libane/ane_m2.c ../libane/ane_v3.c ../libane/ane.c -lm
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
	rm -f ane-run ane-session ane-selfcheck test_libane_ioctl test_libane_t6050 test_libane_v3 test_ane_stats test_t6021_pool test_ane_session_cache test_ane_session_cache_lsan test_ane_session_state test_ane_bo_probe test_t6021_prog_lookup test_t6021_prog_lookup_a64 test_sha256_a64.log test_check_band test_ane_timing test_cpu_floor test_cpu_floor_serve
	rm -f test_libane_h18_resident

# Install the ANE smoke userspace: the program the qualification kit calls by
# name (bare `omarchy-ane-smoke`), the runner it drives, and the add fixtures
# it reads. Without this a source install leaves the kit's `ane` step at exit
# 127 (command missing) instead of a measured exit 0 or an honest exit 2.
SMOKE_BIN = ${DESTDIR}/usr/bin
SMOKE_FIXTURES = ${DESTDIR}/usr/share/omarchy-ane/fixtures

.PHONY: install-smoke uninstall-smoke
install-smoke:
	# Refuse to replace package-owned files: pacman would still attribute
	# them, -Qkk would report mismatches, and the next package upgrade would
	# silently restore the packaged copies. Skipped under DESTDIR (packaging
	# builds) and overridable with FORCE=1.
	@if [ -z "$(DESTDIR)" ] && [ "$(FORCE)" != "1" ] && \
	    command -v pacman >/dev/null 2>&1 && \
	    pacman -Qo /usr/bin/omarchy-ane-smoke >/dev/null 2>&1; then \
		echo "install-smoke: /usr/bin/omarchy-ane-smoke is owned by $$(pacman -Qo /usr/bin/omarchy-ane-smoke | sed 's/.* is owned by //')" >&2; \
		echo "install-smoke: use the package, or rerun with FORCE=1 to overwrite" >&2; \
		exit 1; \
	fi
	# -B: always rebuild, so a copied tree's stale or foreign-arch binary is
	# never installed. The build fails closed if the compiler is missing.
	$(MAKE) -B ane-run
	mkdir -p $(SMOKE_BIN) $(SMOKE_FIXTURES)/h13-anec/add $(SMOKE_FIXTURES)/h14-anec/add
	install -m 755 ane-run $(SMOKE_BIN)/omarchy-ane-run
	install -m 755 ../packaging/omarchy-ane-smoke $(SMOKE_BIN)/omarchy-ane-smoke
	install -m 644 ../fixtures/h13-anec/add/program-0.anec $(SMOKE_FIXTURES)/h13-anec/add/program-0.anec
	install -m 644 ../fixtures/h14-anec/add/program-0.anec $(SMOKE_FIXTURES)/h14-anec/add/program-0.anec

uninstall-smoke:
	rm -f $(SMOKE_BIN)/omarchy-ane-smoke $(SMOKE_BIN)/omarchy-ane-run
	rm -f $(SMOKE_FIXTURES)/h13-anec/add/program-0.anec $(SMOKE_FIXTURES)/h14-anec/add/program-0.anec
