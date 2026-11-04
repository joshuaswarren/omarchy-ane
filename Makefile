.PHONY: all libane tools check install clean

all: libane tools

libane:
	make -C libane libane

tools: libane
	make -C tools tools

check: tools
	make -C tools check

install:
	make -C libane install
	make -C bindings install

clean:
	make -C libane clean
	make -C tools clean
