CC = gcc
CFLAGS = -I../libane -I../ane/src/uapi/drm -Wall -Werror -Wextra \
	-Wdeclaration-after-statement -O3 -std=gnu99

LIB = ../libane/libane.a

.PHONY: tools check clean

tools: ane-run ane-selfcheck

$(LIB):
	$(MAKE) -C ../libane libane

ane-run: ane-run.c ane_f16_add.h $(LIB) ../libane/ane.h ../libane/ane_m2.h
	$(CC) $(CFLAGS) -o $@ ane-run.c $(LIB) -lm

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

check: ane-selfcheck test_libane_ioctl
	./ane-selfcheck ../fixtures/h14-anec
	./test_libane_ioctl ../fixtures/h14-anec

clean:
	rm -f ane-run ane-selfcheck test_libane_ioctl
