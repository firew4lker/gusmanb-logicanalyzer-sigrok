# libsigrok driver for gusmanb LogicAnalyzer (firmware 6.0)

> Implementation status, usage and the hardware checklist are in [README.md](README.md).

## Decisions

| Topic | Decision |
|---|---|
| libsigrok base | **0.5.2 release** (what Arch ships), using only API that also exists on git master |
| Transport | **USB CDC serial only** (VID:PID `1209:3020`). WiFi/TCP is out of scope for now |
| Firmware | **Stock 6.0**, no firmware changes |
| Test hardware | **Pico 2** (RP2350, 384 KB buffer), plus a fake device on a pty |
| Driver name | `gusmanb-logicanalyzer`, in `src/hardware/gusmanb-logicanalyzer/` |
| License | GPLv3+. libsigrok and the upstream project are both GPLv3 |

Reference sources (local):
- `upstream/`: gusmanb/logicanalyzer at tag `v6.0.0.1`. Firmware is in `Firmware/LogicAnalyzer_V2`, the C# reference driver in `Software/LogicAnalyzer/SharedDriver/LogicAnalyzerDriver.cs`.
- `libsigrok/`: libsigrok git master. The tag `libsigrok-0.5.2` is the build base. `src/hardware/raspberrypi-pico/` is the closest existing driver (also a Pico serial logic analyzer that reads back a buffer) and serves as the structural template.

## What the device can and can't do (constraints on the design)

- **It always needs a trigger.** There is no untriggered or immediate mode in the firmware. *(Superseded after hardware testing: with no trigger the driver now triggers on "EXT input low", which fires at once. See README.)*
- **Whole-buffer readback, no streaming.** The device captures into RAM and sends the full buffer when the capture ends. The maximum sample count depends on the highest enabled channel: 8, 16 or 24 channels use 1, 2 or 4 bytes per sample. On the Pico 2 that gives 393216, 196608 or 98304 samples.
- **The "edge" trigger is level-sensitive.** The PIO fires as soon as the pin is at the level (`jmp pin`). If the line is already high when the capture starts, a "rising" trigger fires immediately. This must be documented in the driver.
- **Pattern triggers need GPIO0 and GPIO1 shorted**, as the upstream README requires. Patterns are limited to consecutive channels within channels 0–15. A fast trigger is at most 5 bits; a complex trigger is at most 16 bits.
- **Blast mode** runs at a single fixed rate (`BLASTFREQ`), has no pre-trigger samples and supports edge triggers only.
- **Burst / loop capture with timestamps** has no sigrok equivalent. It is out of scope, so the driver always sends `loopCount=0` and `measure=0`. This also avoids the firmware hang where `measure=1` with `loopCount=0` blocks the capture.
- **Turbo builds:** the device reports its own `FREQ`/`BLASTFREQ`, so turbo builds need no special handling.

## Protocol summary (what the driver implements)

Host to device frame: `55 AA <payload> AA 55`. Any `AA`, `55` or `F0` byte inside the payload is sent as `F0` followed by the byte XOR `F0`. The device replies with ASCII lines ending in `\n`.

| Cmd | Payload | Reply |
|---|---|---|
| `00` identify | none | `LOGIC_ANALYZER_<BOARD>_V<maj>_<min>`, `FREQ:n`, `BLASTFREQ:n`, `BUFFER:n`, `CHANNELS:n` |
| `01` capture | 48-byte `CAPTURE_REQUEST` (layout below) | `CAPTURE_STARTED` or `CAPTURE_ERROR` |
| `05` / `06` blink | none | `BLINKON` / `BLINKOFF`. Optional, could map to an "identify" action later |
| any byte while capturing | | cancels the capture. No data is sent afterwards |

`CAPTURE_REQUEST` is little-endian with natural alignment. Pack it by hand into a `uint8_t[48]`; never `memcpy` a C struct.

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | triggerType | 0 edge, 1 complex, 2 fast, 3 blast |
| 1 | 1 | trigger | channel index (24 = EXT input), or pattern base channel |
| 2 | 1 | inverted / count | edge: 1 = falling (low). Pattern: bit count |
| 3 | 1 | pad | 0 |
| 4 | 2 | triggerValue | pattern, bit 0 = base channel |
| 6 | 24 | channels[] | channel numbers to capture, in order |
| 30 | 1 | channelCount | |
| 31 | 1 | pad | 0 |
| 32 | 4 | frequency | Hz |
| 36 | 4 | preSamples | |
| 40 | 4 | postSamples | |
| 44 | 1 | loopCount | always 0 |
| 45 | 1 | measure | always 0 |
| 46 | 1 | captureMode | 0/1/2 = 8/16/24 channels |
| 47 | 1 | pad | 0 |

The capture result comes after `CAPTURE_STARTED` and arrives whenever the trigger fires:
1. `u32 N` (sample count).
2. `N × {1,2,4}` bytes of samples. Bit *i* of each sample is the *i*-th entry of `channels[]`, not the physical channel number.
3. `u8 stampCount`.
4. `stampCount × u32` timestamps, sent only when `stampCount > 1`. This is always 0 for us, but the parser must still handle it.

## Mapping sigrok concepts to the device

| sigrok | Device |
|---|---|
| Channels `D0`–`D23` | Channel 0–23. `channels[]` = the enabled channels in ascending order; `captureMode` comes from the highest enabled index |
| `SR_CONF_SAMPLERATE` | `frequency`. List 1-2-5 steps from about `MaxFreq*2/65535` (min, as in the C# driver) up to `FREQ`, plus `BLASTFREQ`. Choosing `BLASTFREQ` (> `FREQ`) selects blast mode |
| `SR_CONF_LIMIT_SAMPLES` | `preSamples + postSamples`, checked against the per-mode maximum when acquisition starts |
| `SR_CONF_CAPTURE_RATIO` | `preSamples = limit*ratio/100`, clamped to [2, maxSamples/10] (same limits as the official app). `postSamples` ≥ 2. Forced to 0 in blast mode |
| `SR_CONF_TRIGGER_MATCH` | `ZERO, ONE, RISING, FALLING` (see the trigger rules below) |
| `SR_DF_TRIGGER` | sent after `preSamples` samples |
| `SR_DF_LOGIC` | unitsize = `ceil(highest enabled channel+1 / 8)` (1–3). Device bits are remapped to the sigrok channel bits |

Trigger rules (single stage only; anything else returns `SR_ERR_ARG` with a clear log message):
1. **One channel, `RISING` or `ONE`** → edge trigger, `inverted=0`. **`FALLING` or `ZERO`** → `inverted=1`. Any channel 0–23 works.
2. **Several channels, all `ZERO`/`ONE`, consecutive, within 0–15:**
   - fast trigger if `base+count ≤ 5` (the official app's rule), otherwise
   - complex trigger if `count ≤ 16`.
   - Apply the C# pre/post offset correction (`ComposeRequest`, delay 3 or 5 cycles at `FREQ`).
3. **Blast rate selected:** only rule 1 is allowed.
4. **No trigger:** error out. The device can't capture without one.
5. **Optional extra:** a device-specific trigger source on channel 24 (EXT) for chaining analyzers. Postpone unless needed.

## Driver structure

Follow the layout and patterns of `raspberrypi-pico`:

- **`protocol.h`**: `struct dev_context` holding the device info (version, board, max/blast freq, buffer, channels), the current settings (samplerate, limit, capture ratio), the acquisition state machine and the receive buffer.
- **`protocol.c`**:
  - `gla_frame_encode()` (escaping), `gla_send_cmd()`, `gla_read_line()` (blocking with a timeout).
  - `gla_identify()`: send `00`, parse the 5 lines, check that the version is V6_0 or newer.
  - `gla_build_request()`: covers the trigger-rule mapping and the 48-byte packing.
  - `gla_receive_data()`: the `serial_source_add` callback, a state machine with states `WAIT_LEN → SAMPLES → STAMP_COUNT → STAMPS → DONE`. Samples are converted and sent in chunks (for example 4 KB), and `SR_DF_TRIGGER` is inserted at the pre-trigger boundary.
- **`api.c`**:
  - `scan()`: requires `conn=` (e.g. `/dev/ttyACM0`), defaults to `serialcomm=115200/8n1` (the speed is ignored by CDC, but DTR/RTS must be set), then identifies the device.
  - `config_get/set/list`, `dev_open/close` (`std_serial_dev_open/close`).
  - `dev_acquisition_start()`: validate, send `01`, wait up to 10 s for `CAPTURE_STARTED`, send the header, add the serial source.
  - `dev_acquisition_stop()`: if no data has arrived yet, send `0xFF`, wait about 2.5 s for the device's 1 s polling loop, then flush input, remove the source and send `SR_DF_END`.

Things to watch out for:
- At boot the firmware waits a board-specific delay of up to about 2 s, then 1 s more. `scan()` should retry identify once with a longer timeout.
- Cancel is detected only on a 1 s poll, so expect up to about 2 s of latency. Don't reopen the port the way the C# app does unless flushing turns out not to be enough.
- The device can wait for the trigger indefinitely, so there's no read timeout in the receive state machine. `sigrok-cli` users stop with Ctrl-C.
- Workaround for the firmware blast off-by-one: in blast mode the first sample is lost and the last sample is a 0. Either send `N-1` samples dropping the last one, or note the issue. Confirm on hardware first.

## API compatibility (0.5.2 ↔ master)

Use only functions that exist in both: `serial_*`, `std_serial_dev_*`, `std_scan_complete`, `std_session_send_df_header/end`, `std_gvar_*`, `sr_session_trigger_get`, `sr_channel_new`.

Two spots need a small compat shim (`#ifdef` on `SR_PACKAGE_VERSION_*`, or simply written by hand):
- `std_session_send_df_trigger()` exists only on master. On 0.5.2, build the `SR_DF_TRIGGER` packet by hand.
- `feed_queue_logic_*` exists only on master. Don't use it; send `SR_DF_LOGIC` packets directly.

## Build and integration

1. Add the driver to a 0.5.2 tree: in `configure.ac`, `SR_DRIVER([gusmanb LogicAnalyzer], [gusmanb-logicanalyzer], [serial_comm])`; in `Makefile.am`, add the 3 sources.
2. For development, build into `~/.local/sigrok` and run the system `sigrok-cli` / `pulseview` with `LD_LIBRARY_PATH`. The SONAME is unchanged, so they pick up the new library.
3. For daily use, write an Arch `PKGBUILD` (`libsigrok-gla`, `provides/conflicts=libsigrok`) that applies the driver patch to the 0.5.2 tarball.
4. Later, for upstream: rebase onto master, switch to `std_session_send_df_trigger`, and possibly `feed_queue_logic`, then send the patch to the sigrok mailing list.

## Testing

1. **Fake device** (`tools/fake_gla.py`): a Python program on a pty (`os.openpty`) that implements commands 00/01 and the cancel byte. It checks the 48-byte request, generates known patterns (counter, square waves on given channels) and supports a "trigger after X s" delay and the stamp-count byte. Run the driver against it with `sigrok-cli -d gusmanb-logicanalyzer:conn=/dev/pts/N`.
2. **Unit-level checks:** frame escaping (bytes AA/55/F0 inside the payload), request packing (compare byte-for-byte with C# `Marshal.StructureToPtr` output, using the field offsets above), and bit remapping for sparse channel selections (e.g. D3, D9, D20 → 24-channel mode).
3. **Pico 2 hardware matrix:**
   - 8, 16 and 24-channel modes at minimum and maximum samples.
   - Rising and falling edge triggers.
   - Fast and complex patterns (GPIO0–GPIO1 jumper fitted).
   - Blast mode.
   - Capture ratio 0% and 10%.
   - Cancel while waiting for the trigger.
   - Reconnect after cancel.
   - PulseView with an I²C/UART decoder on real traffic.
   - Compare against the official app on the same signal.

## Milestones

1. **M1 – Identify:** `sigrok-cli --scan` lists the device with its version and 24 channels (on the fake device and on the Pico 2).
2. **M2 – Edge capture, 8 channels:** a capture with a rising/falling trigger and pre-trigger samples saves a correct `.sr` file.
3. **M3 – All channel modes and bit remapping:** sparse channel selections, 16 and 24 channels.
4. **M4 – Pattern triggers:** fast and complex, with offset correction.
5. **M5 – Blast mode and cancel:** acquisition stop, error paths, clear messages for unsupported trigger setups.
6. **M6 – Packaging:** Arch PKGBUILD, a README for the driver (limitations, GPIO0–GPIO1 jumper), and a short PulseView how-to.
7. **M7 (optional) – Upstream:** port to master and submit.

## Open items (decide during implementation)
- Whether to expose blink (cmd 05/06) as a custom config key. Probably skip it.
- Whether to keep the official app's 10% pre-trigger cap or allow up to the firmware's real limit (`pre + post ≤ buffer`). Start with the cap and test going beyond it on hardware.
- WiFi/TCP: possible later on master via `sr_tcp_*`. The protocol is identical apart from the transport.
