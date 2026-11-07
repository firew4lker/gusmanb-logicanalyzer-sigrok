#!/bin/sh
# Build libsigrok 0.5.2 + the gusmanb-logicanalyzer driver and install it
# into ~/.local/sigrok (or $PREFIX). Only the C library and the C++
# bindings (needed by PulseView) are built; nothing system-wide is touched.
#
#   tools/build-dev.sh            prepare tree, incremental build + install
#   tools/build-dev.sh --reconf   also re-run autogen/configure
set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${PREFIX:-$HOME/.local/sigrok}

SRC=$("$TOP/tools/prepare-libsigrok.sh" libsigrok-0.5.2)
cd "$SRC"
if [ "${1:-}" = "--reconf" ] || [ ! -f Makefile ]; then
	./autogen.sh >/dev/null 2>&1
	./configure --prefix="$PREFIX" --disable-python --disable-ruby \
		--disable-java --enable-cxx >/dev/null
	grep -q "^HW_GUSMANB_LOGICANALYZER_TRUE=''" config.log ||
		{ echo "driver not enabled by configure" >&2; exit 1; }
fi
make -j"$(nproc)"
make install >/dev/null
echo "Installed into $PREFIX (use tools/sigrok-cli-gla / tools/pulseview-gla)"
