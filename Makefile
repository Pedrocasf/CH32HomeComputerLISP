all : flash

TARGET:=main
TARGET_MCU:=CH32V002
ADDITIONAL_C_FILES:=video_textmode.c console_textmode.c basic_runtime.c lisp.c
CH32FUN_PATH ?= ./ch32fun

# The V002 family names TIM bits differently from the V003 (see v002_compat.h).
# Inert when building for CH32V003.
EXTRA_CFLAGS+=-include v002_compat.h

# The V002's QingKe V2C core has hardware multiply (Zmmul). ch32fun.mk only
# enables it on gcc >= 13, but the WCH toolchain (riscv-wch-elf-gcc 12.2.0)
# accepts rv32ec_zmmul and emits a real mul. EXTRA_CFLAGS lands after
# CFLAGS_ARCH on the command line, so this -march wins.
ifeq ($(TARGET_MCU),CH32V002)
EXTRA_CFLAGS+=-march=rv32ec_zmmul
endif

ifeq ("$(wildcard $(CH32FUN_PATH)/ch32fun/ch32fun.mk)","")
$(error Could not find ch32fun. Run 'git submodule update --init --recursive' or set CH32FUN_PATH=/path/to/ch32fun)
endif

include $(CH32FUN_PATH)/ch32fun/ch32fun.mk

flash : cv_flash
clean : cv_clean
	-rm -f main.bin main.elf main.elf.ltrans0.ltrans.su main.ext.bin main.hex main.lst main.map main_ext.bin
