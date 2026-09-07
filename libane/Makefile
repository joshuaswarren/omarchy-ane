LIB_NAME = libane

CC = gcc
CFLAGS = -I. -Wall -Werror -Wextra \
	-Wdeclaration-after-statement \
	-O3 -fPIC -std=gnu99

LIBS = -I/usr/include/libdrm -I../ane/src/uapi/drm

BUILD_DIR = .
SRC_DIR = .

OBJECTS = $(BUILD_DIR)/ane.o

.PHONY: libane install uninstall clean

libane: $(OBJECTS)
	ld -r $(OBJECTS) -o $(BUILD_DIR)/${LIB_NAME}.o
	ar rcs $(BUILD_DIR)/${LIB_NAME}.a $(BUILD_DIR)/${LIB_NAME}.o

$(BUILD_DIR)/%.o : $(SRC_DIR)/%.c ane.h ane_f16.h ../ane/src/uapi/drm/ane_accel.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LIBS) -c $< -o $@

install: libane
	install $(BUILD_DIR)/${LIB_NAME}.a ${DESTDIR}/usr/lib
	mkdir -p ${DESTDIR}/usr/include/${LIB_NAME}
	cp ane.h ane_f16.h ${DESTDIR}/usr/include/${LIB_NAME}

uninstall:
	rm -f ${DESTDIR}/usr/lib/${LIB_NAME}.a
	rm -rf ${DESTDIR}/usr/include/${LIB_NAME}

clean:
	rm -f *.o *.a
