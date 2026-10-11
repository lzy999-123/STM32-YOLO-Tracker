#!/bin/sh
set -eu
# Use the matching ARM/uclibc SDK compiler; keep the result outside source control.
COMPILER=${CC:-/work/toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc}
SOURCE=${1:-../video-qos.c}
OUTPUT=${2:-/work/luckfox-video-qos}
"$COMPILER" -std=c99 -Os -Wall -Wextra -Werror -static -s "$SOURCE" -o "$OUTPUT"
