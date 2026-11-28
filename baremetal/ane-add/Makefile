# BMN-04: build the ane-add payload for the T8103 KIS RAM-chainload path.
#
#   make            -> ane-add-payload.bin (flat image, LOAD_ADDR entry)
#   make check      -> host-side checks (no hardware)
#
# Knobs:
#   LOAD_ADDR  physical address the chainloader drops the image at.
#              Default sits ~512 MiB into DRAM on a 16 GiB T8103.
#   UART_BASE  s5l debug uart. 0x235200000 is the j274 console uart;
#              try 0x235204000 / 0x235208000 if the run is silent.
#   ANEC       fixture to embed (default: the h13-anec add program).

AARCH64		?= aarch64-linux-gnu
CC		:= $(AARCH64)-gcc
OBJCOPY		:= $(AARCH64)-objcopy
LD		:= $(AARCH64)-ld

LOAD_ADDR	?= 0x8100000000
UART_BASE	?= 0x235200000
ANEC		?= ../../fixtures/h13-anec/add/program-0.anec

CFLAGS		:= -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
		   -fno-pic -fno-pie -O2 -Wall -Wextra -mgeneral-regs-only \
		   -DUART_BASE=$(UART_BASE)UL -I.
ASFLAGS		:= -DANEC_PATH='"$(ANEC)"'

OBJS		:= start.o payload.o

all: ane-add-payload.bin

ane-add-payload.elf: link.ld $(OBJS)
	$(LD) --defsym LOAD_ADDR=$(LOAD_ADDR) -T link.ld -o $@ $(OBJS)

ane-add-payload.bin: ane-add-payload.elf
	$(OBJCOPY) -O binary $< $@

start.o: start.S
	$(CC) $(CFLAGS) $(ASFLAGS) -c -o $@ $<

payload.o: payload.c regs.h
	$(CC) $(CFLAGS) -c -o $@ $<

check:
	python3 check_payload.py

clean:
	rm -f ane-add-payload.elf ane-add-payload.bin $(OBJS)

.PHONY: all check clean
