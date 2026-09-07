CC = gcc
CFLAGS = -I. -I./include -Wall -Werror -Wextra \
	-Wdeclaration-after-statement \
	-O3 -std=gnu99

LIBS = -I../../../libane

.PHONY: libane_python install uninstall clean

libane_python:
	$(MAKE) -C ../../../libane
	$(CC) $(CFLAGS) $(LIBS)\
		-shared -pthread -fPIC -fno-strict-aliasing \
		libane_python.c ../../../libane/libane.a -o libane_python.so

install: libane_python
	install libane_python.so ${DESTDIR}/usr/lib

uninstall:
	rm -f ${DESTDIR}/usr/lib/libane_python.so

clean:
	rm -f *.so
