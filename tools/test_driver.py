#!/usr/bin/env python3
"""
End-to-end tests of the gusmanb-logicanalyzer libsigrok driver against
fake_gla.py, driven through sigrok-cli.

Usage:
  test_driver.py [--libs DIR[:DIR...]] [-k NAME] [-v]

--libs is prepended to LD_LIBRARY_PATH. Fake-device runs need a
libserialport that accepts pseudo-terminals (see tools/README.md), so a
typical call is:
  test_driver.py --libs $SCRATCH/testlibs/lib:$HOME/.local/sigrok/lib

Signal model (see fake_gla.py): physical channel c at sample k is bit c
of (TRIGGER_VALUE - pre + k), so the sample at the trigger position has
the value TRIGGER_VALUE on the physical inputs.
"""

import argparse
import ast
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE = os.path.join(HERE, "fake_gla.py")
TRIGGER_VALUE = 0x100000
BUFFER = 128 * 3 * 1024  # Pico 2


class Fake:
    def __init__(self, tmp, extra=()):
        self.reqfile = os.path.join(tmp, "requests.txt")
        self.logfile = open(os.path.join(tmp, "fake.log"), "w")
        self.proc = subprocess.Popen(
            [sys.executable, "-I", FAKE, "--dump-requests", self.reqfile,
             "--trigger-value", hex(TRIGGER_VALUE), *extra],
            stdout=subprocess.PIPE, stderr=self.logfile, text=True)
        self.path = self.proc.stdout.readline().strip()

    def requests(self):
        if not os.path.exists(self.reqfile):
            return []
        with open(self.reqfile) as f:
            return [ast.literal_eval(l) for l in f if l.strip()]

    def log(self):
        self.logfile.flush()
        with open(self.logfile.name) as f:
            return f.read()

    def close(self):
        self.proc.terminate()
        self.proc.wait()
        self.logfile.close()


def sigrok(fake, args, env, timeout=30):
    cmd = ["sigrok-cli", "-d",
           f"gusmanb-logicanalyzer:conn={fake.path}:serialcomm=115200/8n1",
           *args]
    return subprocess.run(cmd, env=env, capture_output=True, text=True,
                          timeout=timeout)


def capture(fake, env, tmp, channels, triggers, samples, rate="1m",
            ratio=None):
    out = os.path.join(tmp, "out.bin")
    if os.path.exists(out):
        os.unlink(out)
    args = ["--channels", ",".join(f"D{c}" for c in channels),
            "--samples", str(samples), "--config", f"samplerate={rate}",
            "-O", "binary", "-o", out]
    if ratio is not None:
        args[-4:-4] = ["--config", f"captureratio={ratio}"]
    if triggers:
        args += ["--triggers", triggers]
    r = sigrok(fake, args, env)
    data = open(out, "rb").read() if os.path.exists(out) else b""
    return r, data


def check_samples(data, channels, pre, nsamples, unitsize=3, shift=0):
    """Check the data against the fake's counter signal."""
    if len(data) != nsamples * unitsize:
        return f"got {len(data)} bytes, expected {nsamples * unitsize}"
    base = TRIGGER_VALUE - pre + shift
    mask = sum(1 << c for c in channels)
    for k in range(nsamples):
        v = int.from_bytes(data[k * unitsize:(k + 1) * unitsize], "little")
        want = (base + k) & mask
        if v & ~mask:
            return f"sample {k}: bits outside enabled channels: {v:#x}"
        if v != want:
            return f"sample {k}: got {v:#08x}, want {want:#08x}"
    return None


TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


@test
def scan(fake, env, tmp):
    r = sigrok(fake, ["--scan"], env)
    assert "PICO_2 V6_0 with 24 channels" in r.stdout, r.stdout + r.stderr


@test
def edge_rising_8ch(fake, env, tmp):
    r, data = capture(fake, env, tmp, range(8), "D0=r", 1000, ratio=10)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    assert req["ttype"] == 0 and req["trig"] == 0 and req["inv_cnt"] == 0, req
    assert req["mode"] == 0 and req["chans"] == list(range(8)), req
    assert req["pre"] == 100 and req["post"] == 900, req
    assert req["freq"] == 1000000 and req["loops"] == 0 and req["measure"] == 0
    err = check_samples(data, range(8), 100, 1000)
    assert not err, err


@test
def edge_falling_16ch(fake, env, tmp):
    r, data = capture(fake, env, tmp, range(16), "D12=f", 5000, rate="20m",
                      ratio=5)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    assert req["ttype"] == 0 and req["trig"] == 12 and req["inv_cnt"] == 1, req
    assert req["mode"] == 1 and req["pre"] == 250 and req["post"] == 4750, req
    err = check_samples(data, range(16), 250, 5000)
    assert not err, err


@test
def sparse_channels_24ch(fake, env, tmp):
    chans = [3, 9, 20]
    r, data = capture(fake, env, tmp, chans, "D9=1", 3000, ratio=0)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    assert req["mode"] == 2 and req["chans"] == chans, req
    assert req["pre"] == 2, req  # minimum pre-trigger samples
    err = check_samples(data, chans, 2, 3000)
    assert not err, err


@test
def trigger_low_on_other_channel(fake, env, tmp):
    # (sigrok-cli only accepts triggers on enabled channels.)
    r, data = capture(fake, env, tmp, [0, 1, 5], "D5=0", 500, ratio=10)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    assert req["trig"] == 5 and req["inv_cnt"] == 1, req
    assert req["chans"] == [0, 1, 5] and req["mode"] == 0, req
    err = check_samples(data, [0, 1, 5], 50, 500)
    assert not err, err


@test
def fast_pattern(fake, env, tmp):
    r, data = capture(fake, env, tmp, range(8), "D0=1,D1=0,D2=1", 2000,
                      rate="100m", ratio=10)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    # 3 cycles at 100 MHz = 30 ns = 3 samples, +0.3 rounding -> 3.
    assert req["ttype"] == 2 and req["trig"] == 0 and req["inv_cnt"] == 3, req
    assert req["tval"] == 0b101, req
    assert req["pre"] == 200 + 3 and req["post"] == 1800 - 3, req
    err = check_samples(data, range(8), req["pre"], 2000)
    assert not err, err


@test
def complex_pattern(fake, env, tmp):
    trig = ",".join(f"D{c}={'1' if c % 2 else '0'}" for c in range(4, 10))
    r, data = capture(fake, env, tmp, range(16), trig, 2000, rate="10m",
                      ratio=10)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    # 5 cycles at 100 MHz = 50 ns = 0.5 samples at 10 MHz, +0.3 -> 1.
    assert req["ttype"] == 1 and req["trig"] == 4 and req["inv_cnt"] == 6, req
    assert req["tval"] == 0b101010, req
    assert req["pre"] == 201 and req["post"] == 1799, req


@test
def blast_mode(fake, env, tmp):
    r, data = capture(fake, env, tmp, range(8), "D2=r", 4000, rate="200m",
                      ratio=10)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    assert req["ttype"] == 3 and req["pre"] == 0 and req["post"] == 4001, req
    # Firmware shifts by one: we get samples 1..4000 of the capture.
    err = check_samples(data, range(8), 0, 4000, shift=1)
    assert not err, err


@test
def no_trigger_starts_immediately(fake, env, tmp):
    # No trigger: trigger on "EXT input low" (channel 24, idles low) and
    # discard the 2 unfilled pre-trigger samples.
    r, data = capture(fake, env, tmp, range(8), None, 1000, ratio=30)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    assert req["ttype"] == 0 and req["trig"] == 24 and req["inv_cnt"] == 1, req
    assert req["pre"] == 2 and req["post"] == 1000, req
    # Emitted sample k is wire sample k + 2 (counter base = TRIGGER - 2).
    err = check_samples(data, range(8), 0, 1000)
    assert not err, err


@test
def no_trigger_max_samples(fake, env, tmp):
    r, data = capture(fake, env, tmp, range(8), None, BUFFER)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    assert req["pre"] + req["post"] == BUFFER, req
    err = check_samples(data, range(8), 0, BUFFER - 2)
    assert not err, err


@test
def no_trigger_blast(fake, env, tmp):
    r, data = capture(fake, env, tmp, range(8), None, 3000, rate="200m")
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    assert req["ttype"] == 3 and req["trig"] == 24 and req["inv_cnt"] == 1, req
    err = check_samples(data, range(8), 0, 3000, shift=1)
    assert not err, err


@test
def non_consecutive_pattern_is_error(fake, env, tmp):
    r, data = capture(fake, env, tmp, range(8), "D0=1,D2=1", 1000)
    assert "consecutive" in r.stderr, r.stderr
    assert not fake.requests()


@test
def edge_in_pattern_is_error(fake, env, tmp):
    r, data = capture(fake, env, tmp, range(8), "D0=1,D1=r", 1000)
    assert "only 0/1" in r.stderr, r.stderr


@test
def pattern_above_d15_is_error(fake, env, tmp):
    r, data = capture(fake, env, tmp, range(20), "D15=1,D16=1", 1000)
    assert "D0-D15" in r.stderr, r.stderr


@test
def too_many_samples_rejected(fake, env, tmp):
    # sigrok-cli checks the limit from config_list before starting.
    # (PulseView may not; then the driver clamps to the maximum.)
    r, data = capture(fake, env, tmp, [0, 20], "D0=r", 200000, ratio=1)
    assert "98304" in r.stderr, r.stderr
    assert not fake.requests()


@test
def max_samples_24ch(fake, env, tmp):
    r, data = capture(fake, env, tmp, [0, 20], "D0=r", BUFFER // 4, ratio=1)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    assert req["pre"] + req["post"] == BUFFER // 4 and req["mode"] == 2, req
    err = check_samples(data, [0, 20], req["pre"], BUFFER // 4)
    assert not err, err


@test
def pre_trigger_capped(fake, env, tmp):
    r, data = capture(fake, env, tmp, range(8), "D0=r", 100000, ratio=90)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    assert req["pre"] == BUFFER // 10, req


@test
def large_capture_8ch(fake, env, tmp):
    r, data = capture(fake, env, tmp, range(8), "D7=r", BUFFER, ratio=10)
    assert r.returncode == 0, r.stderr
    req = fake.requests()[-1]
    err = check_samples(data, range(8), req["pre"], BUFFER)
    assert not err, err


STOPTEST = os.environ.get("GLA_STOPTEST")


@test
def stop_while_waiting(fake, env, tmp):
    """sr_session_stop() while waiting for the trigger (PulseView Stop)."""
    assert STOPTEST, "set GLA_STOPTEST to the compiled tools/stoptest.c"
    fake.close()
    fake2 = Fake(tmp, ["--trigger-delay", "60"])
    try:
        t0 = time.monotonic()
        r = subprocess.run([STOPTEST, fake2.path, "115200/8n1", "1000"],
                           env=env, capture_output=True, text=True, timeout=30)
        dt = time.monotonic() - t0
        assert r.returncode == 0, r.stderr
        assert "end=1 samples=0" in r.stderr, r.stderr
        assert dt < 6, f"stop took {dt:.1f} s"
        assert "capture cancelled" in fake2.log(), fake2.log()
        # The device must be usable right afterwards.
        r2 = sigrok(fake2, ["--scan"], env)
        assert "PICO_2" in r2.stdout, r2.stdout + r2.stderr
    finally:
        fake2.close()


@test
def killed_while_waiting_recovers(fake, env, tmp):
    """sigrok-cli 0.7.2 dies on Ctrl-C without stopping the device."""
    fake.close()
    fake2 = Fake(tmp, ["--trigger-delay", "2"])
    try:
        cmd = ["sigrok-cli", "-d",
               f"gusmanb-logicanalyzer:conn={fake2.path}:serialcomm=115200/8n1",
               "--channels", "D0", "--samples", "1000", "--triggers", "D0=r",
               "-O", "binary", "-o", os.path.join(tmp, "x.bin")]
        p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        p.kill()
        p.wait()
        # Device still armed; its trigger fires later and dumps data.
        time.sleep(2.0)
        r, data = capture(fake2, env, tmp, range(8), "D3=r", 2000, ratio=10)
        assert r.returncode == 0, r.stderr
        err = check_samples(data, range(8), 200, 2000)
        assert not err, err
    finally:
        fake2.close()


@test
def armed_device_recovers_on_scan(fake, env, tmp):
    """Scan while the device still waits for a trigger (cancel by identify)."""
    fake.close()
    fake2 = Fake(tmp, ["--trigger-delay", "60"])
    try:
        cmd = ["sigrok-cli", "-d",
               f"gusmanb-logicanalyzer:conn={fake2.path}:serialcomm=115200/8n1",
               "--channels", "D0", "--samples", "1000", "--triggers", "D0=r",
               "-O", "binary", "-o", os.path.join(tmp, "x.bin")]
        p = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        p.kill()
        p.wait()
        r = sigrok(fake2, ["--scan"], env)
        assert "PICO_2" in r.stdout, r.stdout + r.stderr
    finally:
        fake2.close()


@test
def parser_desync_recovers(fake, env, tmp):
    """A stray 0x55 left in the firmware's frame parser (seen on hardware)."""
    import termios
    import tty
    fd = os.open(fake.path, os.O_RDWR | os.O_NOCTTY)
    try:
        tty.setraw(fd)
        os.write(fd, b"\x55")
        # Sanity check of the emulation: a plain frame now gets no answer.
        os.write(fd, b"\x55\xaa\x00\xaa\x55")
        time.sleep(0.5)
        termios.tcflush(fd, termios.TCIFLUSH)
        import select
        r, _, _ = select.select([fd], [], [], 0.5)
        assert not r, "fake device answered a frame while desynced"
    finally:
        os.close(fd)
    t0 = time.monotonic()
    r = sigrok(fake, ["--scan"], env)
    assert "PICO_2" in r.stdout, r.stdout + r.stderr
    assert time.monotonic() - t0 < 2, "recovery needed a retry"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--libs", default="")
    ap.add_argument("-k", help="run only tests containing this string")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()

    env = dict(os.environ)
    env["PYTHONWARNINGS"] = "ignore"
    if a.libs:
        env["LD_LIBRARY_PATH"] = a.libs + (":" + env["LD_LIBRARY_PATH"]
                                           if env.get("LD_LIBRARY_PATH") else "")
    failed = 0
    for t in TESTS:
        if a.k and a.k not in t.__name__:
            continue
        with tempfile.TemporaryDirectory() as tmp:
            fake = Fake(tmp)
            t0 = time.monotonic()
            try:
                t(fake, env, tmp)
                status = "ok"
            except AssertionError as e:
                status = "FAIL"
                failed += 1
                msg = str(e)
            except subprocess.TimeoutExpired as e:
                status = "FAIL"
                failed += 1
                msg = f"timeout: {e}"
            finally:
                log = fake.log() if not fake.logfile.closed else ""
                fake.close()
            print(f"{status:4} {t.__name__} ({time.monotonic() - t0:.1f} s)")
            if status != "ok":
                print("     " + msg[:2000].replace("\n", "\n     "))
                if a.v:
                    print("     fake log:\n" + log)
    print(f"{'FAILED' if failed else 'PASSED'}: {failed} failure(s)")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
