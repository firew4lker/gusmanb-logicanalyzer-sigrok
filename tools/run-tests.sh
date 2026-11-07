#!/bin/sh
# Run the end-to-end driver tests against the fake device.
# Needs: tools/build-dev.sh and tools/build-test-libserialport.sh done once.
#   tools/run-tests.sh [-k NAME] [-v]
set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${PREFIX:-$HOME/.local/sigrok}
TESTLIBS=$TOP/build/testlibs/lib

if [ ! -e "$TESTLIBS/libserialport.so.0" ]; then
	echo "Missing test libserialport, run tools/build-test-libserialport.sh" >&2
	exit 1
fi

mkdir -p "$TOP/build"
cc -Wall -Wextra -o "$TOP/build/stoptest" "$TOP/tools/stoptest.c" \
	$(PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig pkg-config --cflags --libs libsigrok)

GLA_STOPTEST=$TOP/build/stoptest exec python3 -I "$TOP/tools/test_driver.py" \
	--libs "$TESTLIBS:$PREFIX/lib" "$@"
