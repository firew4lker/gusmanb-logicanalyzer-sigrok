#!/usr/bin/env python3
"""
Fake gusmanb LogicAnalyzer (firmware V6_0) on a pseudo-terminal.

Emulates the USB CDC protocol closely enough to develop and test the
libsigrok driver without hardware:

  * framing 55 AA ... AA 55 with F0 escaping,
  * cmd 0 (identify), 1 (capture), 4 (bootloader), 5/6 (blink),
  * capture validation like the firmware (CAPTURE_ERROR on bad params),
  * "first byte cancels" while waiting for the trigger (polled every 1 s,
    like the firmware's 2 s LED loop, shortened); like the firmware, the
    remaining bytes then go to the frame parser,
  * the frame parser's exact resync behaviour (a stray 0x55 desyncs it),
  * "\\r\\n" line endings (Pico SDK stdio),
  * channel re-packing (bit i of each sample = requested channel i),
  * the blast-mode off-by-one bug of firmware 6.0 (disable with --no-blast-bug).

Signal model: the physical input at sample k is the 24-bit counter
(BASE + k) & 0xFFFFFF, where BASE is chosen so that the trigger sample
(k == preSamples) has the value TRIGGER_VALUE (default 0x100000).
So physical channel c at sample k is bit c of the counter.

Usage:
  fake_gla.py [--link PATH] [--board PICO_2] [--trigger-delay 0.5] ...
It prints the pty path (and creates the symlink if --link is given).
"""

import argparse
import fcntl
import os
import select
import struct
import sys
import termios
import time
import tty

REQ_FMT = "<BBBxH24sBxIIIBBBx"
REQ_SIZE = struct.calcsize(REQ_FMT)
assert REQ_SIZE == 48, REQ_SIZE


def log(*a):
    print("[fake]", *a, file=sys.stderr, flush=True)


class FakeDevice:
    def __init__(self, fd, args):
        self.fd = fd
        self.args = args
        self.buf = bytearray()
        self.capture = None  # pending capture (request dict + start time)

    # ---------------------------------------------------------------- io
    def write(self, data):
        if isinstance(data, str):
            # Pico SDK stdio converts "\n" to "\r\n" (raw data is not).
            if not self.args.lf_only:
                data = data.replace("\n", "\r\n")
            data = data.encode()
        view = memoryview(data)
        while view:
            n = os.write(self.fd, view[:4096])
            view = view[n:]

    # ----------------------------------------------------------- framing
    def feed(self, data):
        for b in data:
            self.buf.append(b)
            n = len(self.buf)
            if n == 1 and self.buf[0] != 0x55:
                self.buf.clear()
            elif n == 2 and self.buf[1] != 0xAA:
                self.buf.clear()
            elif n >= 256:
                self.write("ERR_MSG_OVERFLOW\n")
                self.buf.clear()
            elif n > 2 and self.buf[-2] == 0xAA and self.buf[-1] == 0x55:
                raw = bytes(self.buf)
                self.buf.clear()
                self.handle_frame(self.unescape(raw))

    @staticmethod
    def unescape(raw):
        out = bytearray()
        i = 0
        while i < len(raw):
            if raw[i] == 0xF0 and i + 1 < len(raw):
                out.append(raw[i + 1] ^ 0xF0)
                i += 2
            else:
                out.append(raw[i])
                i += 1
        return bytes(out)

    # ---------------------------------------------------------- commands
    def handle_frame(self, msg):
        # msg = 55 AA cmd payload... AA 55
        cmd = msg[2]
        payload = msg[3:-2]
        log(f"cmd {cmd} payload {len(payload)} bytes")
        a = self.args
        if cmd == 0:
            if len(msg) != 5:
                self.write("ERR_UNKNOWN_MSG\n")
                return
            self.write(f"LOGIC_ANALYZER_{a.board}_{a.version}\n")
            self.write(f"FREQ:{a.freq}\n")
            self.write(f"BLASTFREQ:{a.blast_freq}\n")
            self.write(f"BUFFER:{a.buffer}\n")
            self.write(f"CHANNELS:{a.channels}\n")
        elif cmd == 1:
            self.start_capture(payload)
        elif cmd == 4:
            self.write("RESTARTING_BOOTLOADER\n")
        elif cmd == 5:
            self.write("BLINKON\n")
        elif cmd == 6:
            self.write("BLINKOFF\n")
        elif cmd in (2, 3):
            self.write("ERR_UNSUPPORTED\n")
        else:
            self.write("ERR_UNKNOWN_MSG\n")

    def start_capture(self, payload):
        if len(payload) < REQ_SIZE:
            log(f"short capture request ({len(payload)} bytes)")
            self.write("CAPTURE_ERROR\n")
            return
        (ttype, trig, inv_cnt, tval, chans, ccount, freq, pre, post,
         loops, measure, mode) = struct.unpack(REQ_FMT, payload[:REQ_SIZE])
        chans = list(chans[:ccount])
        req = dict(ttype=ttype, trig=trig, inv_cnt=inv_cnt, tval=tval,
                   chans=chans, freq=freq, pre=pre, post=post,
                   loops=loops, measure=measure, mode=mode)
        log("capture request:", req)
        self.last_request = req
        if self.args.dump_requests:
            with open(self.args.dump_requests, "a") as f:
                f.write(repr(req) + "\n")

        a = self.args
        bps = {0: 1, 1: 2, 2: 4}.get(mode)
        ok = bps is not None
        max_samples = a.buffer // bps if ok else 0
        if ok and ttype in (0,):
            ok = (pre + post * (loops + 1) <= max_samples and freq <= a.freq
                  and 1 <= ccount <= 24 and trig <= 24)
        elif ok and ttype in (1, 2):
            ok = (pre + post <= max_samples and freq <= a.freq
                  and ccount <= 24 and trig <= 15 and 1 <= inv_cnt
                  and inv_cnt <= (16 if ttype == 1 else 5)
                  and trig + inv_cnt <= 16)
        elif ok and ttype == 3:
            ok = (post <= max_samples and freq <= a.blast_freq
                  and 1 <= ccount <= 24 and trig <= 24)
        else:
            ok = False
        # Channels must fit the mode (firmware silently loses them otherwise).
        if ok and max(chans, default=0) >= 8 * bps and bps != 4:
            log("WARNING: channel outside capture mode width; data would be lost")
        if not ok:
            self.write("CAPTURE_ERROR\n")
            return
        self.write("CAPTURE_STARTED\n")
        self.capture = (req, time.monotonic())

    def poll_capture(self, got_cancel):
        if not self.capture:
            return
        req, t0 = self.capture
        if got_cancel:
            log("capture cancelled")
            self.capture = None
            return
        if time.monotonic() - t0 < self.args.trigger_delay:
            return
        self.capture = None
        self.send_capture(req)

    def send_capture(self, req):
        a = self.args
        bps = {0: 1, 1: 2, 2: 4}[req["mode"]]
        if req["ttype"] == 3:
            pre, total = 0, req["post"]
        else:
            pre, total = req["pre"], req["pre"] + req["post"] * (req["loops"] + 1)
        base = (a.trigger_value - pre) & 0xFFFFFF
        chans = req["chans"]
        mask = (1 << (8 * bps)) - 1

        def packed(k):
            phys = (base + k) & 0xFFFFFF
            v = 0
            for i, c in enumerate(chans):
                if c < 24 and (phys >> c) & 1:
                    v |= 1 << i
            return v & mask

        values = [packed(k) for k in range(total)]
        if req["ttype"] == 3 and not a.no_blast_bug:
            # Firmware 6.0: start position is off by one -> first sample lost,
            # last sample is whatever follows in the (zeroed) buffer.
            values = values[1:] + [0]

        fmt = {1: "B", 2: "H", 4: "I"}[bps]
        time.sleep(0.1)
        self.write(struct.pack("<I", total))
        time.sleep(0.1)
        data = struct.pack(f"<{total}{fmt}", *values)
        self.write(data)
        self.write(b"\x00")  # timestamp count
        log(f"sent {total} samples ({len(data)} bytes)")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--link", help="create a symlink to the pty at this path")
    p.add_argument("--board", default="PICO_2")
    p.add_argument("--version", default="V6_0")
    p.add_argument("--freq", type=int, default=100000000)
    p.add_argument("--blast-freq", type=int, default=200000000)
    p.add_argument("--buffer", type=int, default=128 * 3 * 1024)
    p.add_argument("--channels", type=int, default=24)
    p.add_argument("--trigger-delay", type=float, default=0.3,
                   help="seconds until the trigger fires (default 0.3)")
    p.add_argument("--trigger-value", type=lambda s: int(s, 0), default=0x100000)
    p.add_argument("--no-blast-bug", action="store_true")
    p.add_argument("--lf-only", action="store_true",
                   help="send '\\n' line endings instead of the Pico's '\\r\\n'")
    p.add_argument("--dump-requests", help="append parsed capture requests to file")
    args = p.parse_args()

    master, slave = os.openpty()
    tty.setraw(master)
    tty.setraw(slave)
    path = os.ttyname(slave)
    if args.link:
        try:
            os.unlink(args.link)
        except FileNotFoundError:
            pass
        os.symlink(path, args.link)
    print(path, flush=True)
    log(f"listening on {path}" + (f" ({args.link})" if args.link else ""))

    dev = FakeDevice(master, args)
    cancel_at = None
    pending = b""
    try:
        while True:
            # We keep the slave end open, so the tty is never fully closed
            # and an exclusive lock (TIOCEXCL) of a killed client would
            # stay forever. A real USB CDC port is released on close.
            try:
                fcntl.ioctl(slave, termios.TIOCNXCL)
            except OSError:
                pass
            r, _, _ = select.select([master], [], [], 0.05)
            if r:
                try:
                    data = os.read(master, 4096)
                except OSError:
                    data = b""
                if data:
                    if dev.capture:
                        # Firmware: while a capture is armed it polls about
                        # once per second and consumes exactly ONE byte as
                        # the cancel request; the rest stays in the USB
                        # buffer and goes to the frame parser afterwards.
                        pending += data
                        if cancel_at is None:
                            cancel_at = time.monotonic() + 1.0
                    else:
                        dev.feed(data)
            if dev.capture:
                cancelled = cancel_at is not None and time.monotonic() >= cancel_at
                if cancel_at is None or cancelled:
                    dev.poll_capture(cancelled)
                if cancelled:
                    cancel_at = None
                    pending = pending[1:]  # the cancel byte
            if not dev.capture and pending:
                data, pending = pending, b""
                dev.feed(data)
            if not dev.capture:
                cancel_at = None
    except KeyboardInterrupt:
        pass
    finally:
        if args.link:
            try:
                os.unlink(args.link)
            except OSError:
                pass


if __name__ == "__main__":
    main()
