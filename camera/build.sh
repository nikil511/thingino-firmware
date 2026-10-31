#!/bin/sh
# Build snapsign as a static MIPS32 little-endian binary for Ingenic
# cameras. Needs git and zig (pip install ziglang works: ZIG="python3 -m ziglang").
#   sh camera/build.sh [target]     default target: mipsel-linux-musleabi
set -e
cd "$(dirname "$0")"

RAPTOR_COMMON_SITE=https://github.com/gtxaspec/raptor-common
# Same version thingino master pins in package/thingino-raptor-common
RAPTOR_COMMON_VERSION=ab1f9fd40dc917388449c201a0945c051587f4fb
TARGET=${1:-mipsel-linux-musleabi}
ZIG=${ZIG:-zig}

if [ ! -d raptor-common ]; then
	git init -q raptor-common
	git -C raptor-common fetch -q --depth 1 "$RAPTOR_COMMON_SITE" "$RAPTOR_COMMON_VERSION"
	git -C raptor-common checkout -q FETCH_HEAD
fi

R=raptor-common
M=$R/third_party/monocypher
$ZIG cc -target "$TARGET" -mcpu=mips32r2 -Os -static -s \
	-I"$R/include" -I"$M" \
	-o snapsign snapsign.c "$R/src/rss_jpeg.c" "$M/monocypher.c" "$M/monocypher-ed25519.c"
echo "built $(pwd)/snapsign for $TARGET"
