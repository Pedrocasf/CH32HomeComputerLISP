all : flash

TARGET:=main
TARGET_MCU:=CH32V002
ADDITIONAL_C_FILES:=video_textmode.c console_textmode.c lisp_runtime.c
CH32FUN_PATH ?= ./ch32fun

ifeq ("$(wildcard $(CH32FUN_PATH)/ch32fun/ch32fun.mk)","")
$(error Could not find ch32fun. Run 'git submodule update --init --recursive' or set CH32FUN_PATH=/path/to/ch32fun)
endif

include $(CH32FUN_PATH)/ch32fun/ch32fun.mk

flash : cv_flash
clean : cv_clean
	-rm -f main.bin main.elf main.elf.ltrans0.ltrans.su main.ext.bin main.hex main.lst main.map main_ext.bin
