#!/bin/sh
# Compile the driver against current libsigrok master (C library only),
# to keep it portable for upstream submission. Fetches master first and
# builds in a fresh tree (build/libsigrok-master), takes about a minute.
set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
REPO=$TOP/build/libsigrok.git
DIR=$TOP/build/libsigrok-master

"$TOP/tools/prepare-libsigrok.sh" libsigrok-0.5.2 >/dev/null  # ensures clone
git -C "$REPO" fetch -q origin
[ -e "$DIR/.git" ] && git -C "$REPO" worktree remove --force "$DIR"
"$TOP/tools/prepare-libsigrok.sh" origin/master "$DIR" >/dev/null

cd "$DIR"
./autogen.sh >/dev/null 2>&1
./configure --disable-python --disable-ruby --disable-java \
	--disable-cxx >/dev/null
make -j"$(nproc)" >build.log 2>&1 || { tail -30 build.log; exit 1; }
if grep -E 'gusmanb-logicanalyzer.*(warning|error)' build.log; then
	exit 1
fi
echo "OK: driver builds cleanly against libsigrok master $(git rev-parse --short HEAD)"
