# The binding derivation is header-only and touches no device, so this test
# builds and runs on any host, including x86_64 without an ANE.
all:
	gcc -I. -I../../libane -Wall -Werror -Wextra -std=gnu99 -O2 main.c -o main.out

check: all
	./main.out --self-test

clean:
	rm -f *.out
