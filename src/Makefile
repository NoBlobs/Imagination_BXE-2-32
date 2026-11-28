# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
#
# Build the BXE-2-32 (36.29.52.182) firmware: microMIPS32r2, o32, soft-float.
# GNU toolchain: GCC and binutils for mipsel-elf.
#   make            -> ../build/rogue_36.29.52.182_v1.fw
#   make EXP="-DNAME=value"           (extra defines for experiments; rebuild with -B)
#   make SFH=<path to pvr_rogue_fwif_sf.h> sfid   (regenerate include/sfid.h)
#   make CROSS=<prefix> GCC=<gcc>     (another toolchain; C23 needs GCC 14 or later)

BUILD   ?= ../build
TOOLS   ?= ../tools
CROSS   ?= /opt/rgxcross/bin/mipsel-elf-
GCC     ?= $(CROSS)gcc-16.2.0
STD     ?= gnu23
CC      := $(GCC)
LD      := $(CROSS)ld
OBJDUMP := $(CROSS)objdump

TARGET  := -EL -march=mips32r2 -mmicromips -mabi=32 -msoft-float
CFLAGS  := $(TARGET) -std=$(STD) -O2 -g -ffreestanding -fno-builtin -fno-pic -mno-abicalls \
           -G0 -mlong-calls -fno-common -fno-stack-protector -fno-jump-tables -Wall -Wextra -Werror \
           -fno-delete-null-pointer-checks -mno-check-zero-division \
           -Wno-unused-parameter -Iinclude $(EXP)
ASFLAGS := $(TARGET) -fno-pic -mno-abicalls -G0 -Iinclude

SRCS_C  := main.c util.c kccb.c power.c mmu.c sched.c kick.c gpu_init.c hwr.c stats.c geom.c frag.c pm.c
SRCS_S  := boot.S vectors.S
OBJS    := $(addprefix $(BUILD)/obj/,$(SRCS_S:.S=.o) $(SRCS_C:.c=.o))

ELF     := $(BUILD)/fw.elf
FW      := $(BUILD)/rogue_36.29.52.182_v1.fw

all: $(FW)

$(BUILD)/obj/%.o: %.c include/*.h Makefile | $(BUILD)/obj
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: %.S include/*.h Makefile | $(BUILD)/obj
	$(CC) $(ASFLAGS) -c $< -o $@

$(ELF): $(OBJS) fw.ld
	python3 $(TOOLS)/check-trace-args.py include/sfid.h $(SRCS_C)
	$(LD) -T fw.ld -o $@ $(OBJS)
	$(OBJDUMP) -d $@ > $(BUILD)/fw.dis

$(FW): $(ELF) $(TOOLS)/mkfw.py include/layout.h
	python3 $(TOOLS)/mkfw.py --layout include/layout.h $(ELF) $@

$(BUILD)/obj:
	mkdir -p $@

sfid:
	python3 $(TOOLS)/gen_sfids.py $(SFH) include/sfid.h

clean:
	rm -rf $(BUILD)/obj $(ELF) $(FW) $(BUILD)/fw.dis

.PHONY: all clean sfid
