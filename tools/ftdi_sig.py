#!/usr/bin/env python3
"""
Drive an FT232R (or any USB-UART with RTS/DTR) as a test signal source.

Wiring used for the hardware tests: TX->D0, RTS->D1, DTR->D2, GND->GND.
RTS/DTR pins are active low; this script takes *pin levels* (1 = high).

  ftdi_sig.py levels RTS DTR [SECONDS]
      hold RTS/DTR pins at the given levels (TX idles high)
  ftdi_sig.py uart TEXT [--baud 115200] [--repeat N] [--gap S]
      send TEXT on TX
  ftdi_sig.py sequence STEP_MS RTS,DTR [RTS,DTR ...]
      step through RTS/DTR pin levels, STEP_MS milliseconds each, after a 1 s
      start delay (so a capture can be armed first)
"""

import argparse
import sys
import time

import serial


def open_port(dev, baud):
    s = serial.Serial()
    s.port = dev
    s.baudrate = baud
    # Set the lines before opening so they don't glitch.
    s.rts = False
    s.dtr = False
    s.open()
    return s


def set_levels(s, rts, dtr):
    # Asserted (True) drives the pin low.
    s.rts = not rts
    s.dtr = not dtr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("levels")
    p.add_argument("rts", type=int)
    p.add_argument("dtr", type=int)
    p.add_argument("seconds", type=float, nargs="?", default=10)
    p = sub.add_parser("uart")
    p.add_argument("text")
    p.add_argument("--repeat", type=int, default=1)
    p.add_argument("--gap", type=float, default=0.0)
    p.add_argument("--delay", type=float, default=1.0)
    p = sub.add_parser("sequence")
    p.add_argument("step_ms", type=float)
    p.add_argument("steps", nargs="+")
    a = ap.parse_args()

    s = open_port(a.port, a.baud)
    try:
        if a.cmd == "levels":
            set_levels(s, a.rts, a.dtr)
            time.sleep(a.seconds)
        elif a.cmd == "uart":
            time.sleep(a.delay)
            for _ in range(a.repeat):
                s.write(a.text.encode())
                s.flush()
                time.sleep(a.gap)
        elif a.cmd == "sequence":
            first = [int(v) for v in a.steps[0].split(",")]
            set_levels(s, *first)
            time.sleep(1.0)
            for st in a.steps:
                rts, dtr = (int(v) for v in st.split(","))
                set_levels(s, rts, dtr)
                time.sleep(a.step_ms / 1000)
            time.sleep(1.0)
    finally:
        s.close()


if __name__ == "__main__":
    sys.exit(main())
