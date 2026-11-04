.PHONY: all libane tools check install clean

all: libane tools

libane:
	make -C libane libane

tools: libane
	make -C tools tools

check: tools
	make -C tools check
	python3 ane/test/tm_transient_fault_test.py
	python3 ane/test/tm_close_test.py
	python3 ane/test/tm_recovery_guard_test.py

install:
	make -C libane install
	make -C bindings install

clean:
	make -C libane clean
	make -C tools clean
