#!/bin/sh
# Prepare a libsigrok source tree with the gusmanb-logicanalyzer driver.
#
#   tools/prepare-libsigrok.sh [REF] [DIR]
#
# REF  libsigrok git ref (default: libsigrok-0.5.2)
# DIR  tree to prepare (default: build/libsigrok-0.5.2, build/libsigrok-master, ...)
#
# Clones libsigrok into build/libsigrok.git on first use (set LIBSIGROK_GIT
# to clone from somewhere else, e.g. a local mirror), creates a worktree at
# REF, registers the driver in configure.ac / Makefile.am (idempotent) and
# copies the driver sources from src/hardware/gusmanb-logicanalyzer.
# Prints the tree's path on stdout.
set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
REF=${1:-libsigrok-0.5.2}
NAME=$(echo "${REF#libsigrok-}" | tr / -)
DIR=${2:-$TOP/build/libsigrok-$NAME}
REPO=$TOP/build/libsigrok.git
DRV=gusmanb-logicanalyzer

mkdir -p "$TOP/build"
if [ ! -d "$REPO" ]; then
	git clone -q "${LIBSIGROK_GIT:-git://sigrok.org/libsigrok}" "$REPO" >&2
	git -C "$REPO" remote set-url origin git://sigrok.org/libsigrok
fi

if [ ! -e "$DIR/.git" ]; then
	git -C "$REPO" worktree prune
	git -C "$REPO" worktree add -q --detach "$DIR" "$REF" >&2
fi

cd "$DIR"

# configure.ac: insert after the GMC MH driver (alphabetical order).
if ! grep -q "\[$DRV\]" configure.ac; then
	line=$(grep -n 'SR_DRIVER(\[GMC MH' configure.ac | cut -d: -f1)
	[ -n "$line" ] || { echo "configure.ac: anchor not found" >&2; exit 1; }
	sed -i "${line}a SR_DRIVER([gusmanb LogicAnalyzer], [$DRV], [serial_comm])" configure.ac
fi

# Makefile.am: insert before the GW Instek GDS-800 block.
if ! grep -q "HW_GUSMANB_LOGICANALYZER" Makefile.am; then
	line=$(grep -n '^if HW_GWINSTEK_GDS_800' Makefile.am | cut -d: -f1)
	[ -n "$line" ] || { echo "Makefile.am: anchor not found" >&2; exit 1; }
	sed -i "${line}i if HW_GUSMANB_LOGICANALYZER\\
src_libdrivers_la_SOURCES += \\\\\\
	src/hardware/$DRV/protocol.h \\\\\\
	src/hardware/$DRV/protocol.c \\\\\\
	src/hardware/$DRV/api.c\\
endif" Makefile.am
fi

# Driver sources (copied, so build products stay out of the repository).
mkdir -p "src/hardware/$DRV"
for f in api.c protocol.c protocol.h; do
	cmp -s "$TOP/src/hardware/$DRV/$f" "src/hardware/$DRV/$f" ||
		cp "$TOP/src/hardware/$DRV/$f" "src/hardware/$DRV/$f"
done

echo "$DIR"
