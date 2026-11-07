#!/bin/sh
# Build a TEST-ONLY libserialport 0.1.2 that can open pseudo-terminals,
# so the driver can be exercised against tools/fake_gla.py.
#
# Stock libserialport rejects ptys: it needs a /sys/class/tty entry for
# the port and fails on the modem-line ioctls (TIOCMGET/TIOCMBIS), which
# ptys do not support. This build ignores both for /dev/pts/* only.
# Never install it system-wide; it is loaded via LD_LIBRARY_PATH by
# tools/run-tests.sh.
set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
SRC=$TOP/build/libserialport-test-src
PREFIX=$TOP/build/testlibs

REPO=$TOP/build/libserialport.git

mkdir -p "$TOP/build"
if [ ! -d "$REPO" ]; then
	git clone -q "${LIBSERIALPORT_GIT:-git://sigrok.org/libserialport}" "$REPO"
	git -C "$REPO" remote set-url origin git://sigrok.org/libserialport
fi
rm -rf "$SRC"
git -C "$REPO" worktree prune
git -C "$REPO" worktree add -q -f --detach "$SRC" libserialport-0.1.2

python3 -I - "$SRC/serialport.c" <<'EOF'
import re, sys
p = sys.argv[1]
s = open(p).read()

old = """	if ((ret = get_port_details(port)) != SP_OK) {
		sp_free_port(port);
		return ret;
	}"""
new = """	if ((ret = get_port_details(port)) != SP_OK) {
		/* TEST BUILD: allow pseudo-terminals (no sysfs entry). */
		if (strncmp(portname, "/dev/pts/", 9)) {
			sp_free_port(port);
			return ret;
		}
	}"""
assert s.count(old) == 1
s = s.replace(old, new)

helper = """
#ifndef _WIN32
/* TEST BUILD: pseudo-terminals have no modem lines (ENOTTY). */
static int pty_ok_ioctl(int fd, unsigned long req, int *arg)
{
	int r = ioctl(fd, req, arg);
	if (r < 0 && errno == ENOTTY) {
		if (req == TIOCMGET)
			*arg = 0;
		return 0;
	}
	return r;
}
#endif
"""
anchor = "SP_API enum sp_return sp_get_port_by_name"
s = s.replace(anchor, helper + anchor, 1)
s = s.replace("ioctl(port->fd, TIOCMGET, &data->controlbits)",
              "pty_ok_ioctl(port->fd, TIOCMGET, &data->controlbits)")
s, n = re.subn(r"ioctl\(port->fd, config->(rts|dtr) == SP_(RTS|DTR)_ON \? TIOCMBIS : TIOCMBIC",
               r"pty_ok_ioctl(port->fd, config->\1 == SP_\2_ON ? TIOCMBIS : TIOCMBIC", s)
assert n == 4, n
open(p, "w").write(s)
EOF

cd "$SRC"
./autogen.sh >/dev/null
./configure --prefix="$PREFIX" >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "Test libserialport installed in $PREFIX/lib"
