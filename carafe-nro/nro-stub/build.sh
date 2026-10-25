#!/bin/sh
set -ex
export PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
aarch64-none-elf-gcc -O2 -Wall $ARCH -D__SWITCH__ -I$DEVKITPRO/libnx/include -c source/main.c -o main.o
aarch64-none-elf-gcc -specs=$DEVKITPRO/libnx/switch.specs $ARCH main.o -L$DEVKITPRO/libnx/lib -lnx -o carafe-launcher.elf
elf2nro carafe-launcher.elf carafe-launcher.nro
ls -l carafe-launcher.nro
