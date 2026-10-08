#!/bin/sh
set -e
cd /work/kernel
mkdir -p wifi-deps
cp net/wireless/lib80211*.c wifi-deps/
printf 'obj-m += lib80211.o lib80211_crypt_wep.o lib80211_crypt_ccmp.o\n' > wifi-deps/Makefile
make -j1 ARCH=arm CROSS_COMPILE=/work/toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf- M=wifi-deps modules
cp wifi-deps/*.ko /work/
echo DEPENDENCIES_COMPLETE
