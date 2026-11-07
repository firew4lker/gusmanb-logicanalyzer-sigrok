#!/bin/sh
# Regenerate patches/0001-gusmanb-logicanalyzer-driver.patch (against the
# libsigrok-0.5.2 tag) from src/, copy it to packaging/ and update its
# checksum in packaging/PKGBUILD.
set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
NAME=0001-gusmanb-logicanalyzer-driver.patch
TMP=$TOP/build/patch-tree

rm -rf "$TMP"
"$TOP/tools/prepare-libsigrok.sh" libsigrok-0.5.2 "$TMP" >/dev/null
cd "$TMP"
git add -N src/hardware/gusmanb-logicanalyzer
mkdir -p "$TOP/patches"
git diff -- configure.ac Makefile.am src/hardware/gusmanb-logicanalyzer \
	> "$TOP/patches/$NAME"
cd "$TOP"
git -C build/libsigrok.git worktree remove --force "$TMP"

cp "patches/$NAME" "packaging/$NAME"
sum=$(b2sum "packaging/$NAME" | cut -d' ' -f1)
# The driver patch is the last source, so its checksum is the last entry.
python3 -I - "packaging/PKGBUILD" "$sum" <<'EOF'
import re, sys
path, new = sys.argv[1], sys.argv[2]
s = open(path).read()
sums = re.search(r"b2sums=\((.*?)\)", s, re.S)
entries = re.findall(r"'([0-9a-f]+|SKIP)'", sums.group(1))
old = entries[-1]
s = s[:sums.start(1)] + sums.group(1).replace(old, new) + s[sums.end(1):]
open(path, "w").write(s)
EOF
echo "patches/$NAME: $(grep -c '^+' "patches/$NAME") added lines, b2sum updated in PKGBUILD"
