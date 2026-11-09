# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
#
# Build the BXE-2-32 (36.29.52.182) firmware: microMIPS32r2, o32, soft-float.
#   make            -> ../build/rogue_36.29.52.182_v1.fw
#   make EXP="-DNAME=value"           (extra defines for experiments; rebuild with -B)
#   make SFH=<path to pvr_rogue_fwif_sf.h> sfid   (regenerate include/sfid.h)

BUILD   ?= ../build
TOOLS   ?= ../tools
CC      := clang
LD      := ld.lld
OBJDUMP := llvm-objdump

TARGET  := --target=mipsel-unknown-elf -march=mips32r2 -mmicromips -mabi=32 -msoft-float
CFLAGS  := $(TARGET) -std=c23 -O2 -g -ffreestanding -fno-builtin -fno-pic -mno-abicalls \
           -G0 -mlong-calls -fno-common -fno-stack-protector -fno-jump-tables -Wall -Wextra -Werror \
           -Wno-unused-parameter -Iinclude $(EXP)
ASFLAGS := $(TARGET) -fno-pic -mno-abicalls -G0 -Iinclude -D__ASSEMBLER__

SRCS_C  := main.c util.c kccb.c power.c mmu.c sched.c kick.c gpu_init.c hwr.c
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
	$(TOOLS)/ld-wrap.sh $(LD) -T fw.ld --no-check-sections --noinhibit-exec -o $@ $(OBJS)
	$(OBJDUMP) -d --mattr=+micromips $@ > $(BUILD)/fw.dis

$(FW): $(ELF) $(TOOLS)/mkfw.py include/layout.h
	python3 $(TOOLS)/mkfw.py --layout include/layout.h $(ELF) $@

$(BUILD)/obj:
	mkdir -p $@

sfid:
	python3 $(TOOLS)/gen_sfids.py $(SFH) include/sfid.h

clean:
	rm -rf $(BUILD)/obj $(ELF) $(FW) $(BUILD)/fw.dis

.PHONY: all clean sfid
