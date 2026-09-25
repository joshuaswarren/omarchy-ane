# The kill-race needs a live device (fork + real SUBMIT), so `all` only
# builds; `check` runs it where /dev/accel/accel0 exists. Host-only build
# like the other tests.
all:
	gcc -I. -I../../ane/src/uapi -Wall -Werror -Wextra -std=gnu99 -O2 main.c -o main.out

check: all
	./main.out 1

clean:
	rm -f main.out

.PHONY: all check clean
