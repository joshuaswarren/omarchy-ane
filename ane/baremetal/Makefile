# SPDX-License-Identifier: MIT
# Build the BMN-03 bare-metal payload Image.
#   make            -> bmn03-payload.bin (flat arm64 Image)
#   make disasm     -> human-readable disassembly for review
CROSS ?= aarch64-linux-gnu-
CC     = $(CROSS)gcc
LD     = $(CROSS)ld
OBJCOPY= $(CROSS)objcopy
OBJDUMP= $(CROSS)objdump

CFLAGS = -ffreestanding -nostdlib -nostdinc -fno-pic -fno-stack-protector \
         -fno-builtin -mgeneral-regs-only -O2 -Wall -Wextra

all: bmn03-payload.bin

entry.o: bmn03-payload.S
	$(CC) $(CFLAGS) -c -o $@ $<

payload.o: bmn03-payload.c
	$(CC) $(CFLAGS) -c -o $@ $<

bmn03-payload.elf: entry.o payload.o link.ld
	$(LD) -nostdlib -T link.ld -o $@ entry.o payload.o

bmn03-payload.bin: bmn03-payload.elf
	$(OBJCOPY) -O binary $< $@
	@size=$$(stat -c %s $@); \
	 echo "bmn03-payload.bin: $$size bytes (ARM64 magic at +0x38: $$(xxd -p -l4 -s0x38 $@))"

ZIG   ?= zig

kex0.efi: kex0-efi.zig
	$(ZIG) build-exe -target aarch64-uefi -O ReleaseSmall -femit-bin=$@ $<

kex0-probe.o: kex0-probe.c
	$(CC) $(CFLAGS) -c -o $@ $<

kex0-probe.elf: entry.o kex0-probe.o link.ld
	$(LD) -nostdlib -T link.ld -o $@ entry.o kex0-probe.o

kex0-probe.bin: kex0-probe.elf
	$(OBJCOPY) -O binary $< $@
	@size=$$(stat -c %s $@); \
	 echo "kex0-probe.bin: $$size bytes (ARM64 magic at +0x38: $$(xxd -p -l4 -s0x38 $@))"

disasm: bmn03-payload.elf
	$(OBJDUMP) -d -maarch64 bmn03-payload.elf

clean:
	rm -f entry.o payload.o kex0-probe.o bmn03-payload.elf bmn03-payload.bin \
	      kex0-probe.elf kex0-probe.bin kex0.efi

.PHONY: all clean disasm
