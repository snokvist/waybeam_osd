#!/bin/bash -x
DL="https://github.com/OpenIPC/firmware/releases/download/toolchain/toolchain"
CC=sigmastar-infinity6e
GCC=$PWD/toolchain/$CC/bin/arm-linux-gcc
OUT=lvgltest

if [[ "$1" != *"native"* && "$1" != *"rockhip"* ]]; then
	if [ ! -e toolchain/$CC ]; then
		wget -c -q --show-progress --no-check-certificate $DL.$CC.tgz -P $PWD
		mkdir -p toolchain/$CC
		tar -xf toolchain.$CC.tgz -C toolchain/$CC --strip-components=1 || exit 1
		rm -f $CC.tgz
	fi
fi

if [ ! -e firmware ]; then
	git clone https://github.com/openipc/firmware --depth=1
fi

if [ ! -d lvgl ]; then
	git submodule update --init
fi

DRV=$PWD/firmware/general/package/sigmastar-osdrv-infinity6e/files/lib
make -j8 -B CC=$GCC DRV=$DRV TOOLCHAIN=$PWD/toolchain/$CC OUTPUT=$OUT \
    LVGL_INCLUDE_DEMOS=0 LVGL_INCLUDE_EXAMPLES=0
