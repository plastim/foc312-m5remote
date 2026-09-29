# M5 remote: foc312 on the OSSM M5 Remote, driving a FOC-Stim directly (spec, 2026-09-28)

PlaStim's decisions from the 2026-09-28 conversation. Nothing is built yet. Names are PlaStim's to give.

## Hardware
- The OSSM M5 Remote (ortlof/OSSM-M5-Remote, MarcoB979's BLE fork in `Projects`): M5Stack
  CoreS3 SE (ESP32-S3, 320x240 touch LCD) on a carrier with **4 encoders (2 with push) + 1 MX button**.
  Pins (S3): enc1 5/9, enc2 18/17, enc3 1/2, enc4 7/6; MX button 10; encoder pushes 8 ("left") and 14 ("right").
  Measured with the hwtest firmware (2026-09-28, remote on COM18, ESP32-S3 16 MB flash, MAC 02:00:00:00:00:01):
  | Control | Encoder / pins | Clockwise | Push |
  |---|---|---|---|
  | Knob 1 (master) | enc1, CLK 5 / DT 9 | counts up | pin 8, high = pressed |
  | Knob 2 (level A, position 1's wires) | enc2, 18 / 17 | counts up | - |
  | Knob 3 (level B, position 2's wires) | enc3, 1 / 2 | counts up | - |
  | Knob 4 (MA) | enc4, 7 / 6 | counts up | pin 14, high = pressed |
  | MX button | - | - | pin 10, high = pressed |
  Half-quadrature counting: 2 counts per detent. Directions confirmed by PlaStim on the app: all four count up clockwise.
- **Power button (CoreS3):** a short press turns it on; a 6 s hold turns the power chip off. After that hold, even on
  USB, the ESP32 is only half-powered: black screen, restarting ~6x/s in the bootloader (rst:0x3 RTC_SW_SYS_RST, Saved
  PC 0x403cdada), USB dropping in and out. A **reset press** (side button) brings it back (2026-09-28). Holding reset
  ~3 s (green light while held) = download mode, for flashing when it won't stay on USB. The app now lights the screen
  and shows "stim remote: starting" first thing, so a black screen can only mean no power.
- The OSSM firmware's full-flash backup: `private/m5/ossm-remote-backup-16MB.bin` (16 MB, SHA-256 889bd697...f8a2;
  gitignored, local only). Restore: `esptool --port COMx write-flash 0 <file>`.
- **Licence:** that project is CC BY-SA 4.0. Our firmware is written from scratch (pins are facts, no code reuse), so
  it can carry our own licence. Anyone can build the remote from their PCB.

## Controls
| Control | Run screen | Pattern list | Options (wiring picture) |
|---|---|---|---|
| Knob 1 turn | master volume | scroll | move the highlight |
| Knob 1 press | open patterns | pick | toggle / next for the highlighted item |
| Knob 2 turn | level A (wires in position 1) | jump 10 | position 1's wires: every pair, both ways |
| Knob 3 turn | level B (wires in position 2) | - | position 2's wires: every pair, both ways |
| Knob 4 turn | MA (Multi Adjust, the ET-312 knob) | - | pulse shape, round the list (whatever is highlighted) |
| Knob 4 press | open options | back | back |
| MX button | start / stop | start / stop | start / stop |

- Output keeps running in the pattern and options screens, but **in the menus no knob touches the output**: master,
  MA and the levels hold (PlaStim, 2026-09-28: "in the menus the knobs shouldn't control the volumes"). STOP always
  works, and the menus fall back to the run screen after 20 s untouched.
- **Levels stay with the wires.** Channel swap moves the *patterns* between the wire pairs; knob 2 always sets the
  wires in position 1, knob 3 position 2.
- Levels 0-100 in 1 % steps. No ET-312 Power level and no Advanced menu on the remote (PlaStim: not needed).
- History: first MA on knob 1 and master on knob 2; PlaStim moved master to knob 1 and MA to knob 2 after first use
  (the bars on screen follow knob order), then wanted knob 1 back as the list scroller. 2026-09-29 (PlaStim): knobs
  1-4 = master, level A, level B, MA (MA on 4, under its upright bar on the right edge); in the options 2 / 3 step
  position 1's / 2's wires and 4 the pulse shape, so A and B stay on knobs 2 and 3 everywhere.

## Options screen
A picture (PlaStim: "make polarity, wires and swap graphical"): the four pads in a row, position 1's wires as an orange
bracket above them and position 2's as a cyan one below, each with an arrow for its polarity and the pattern (A/B)
it plays; unconnected pads are crossed out. Buttons underneath for swap, shape, skip-ramp, box and back.
- Polarity per channel (a lead swap = route digit swap on the fork).
- Wire selection (route) per channel: 12 / 21 / 34 / 43 / 23 / 32 / 41 / 14 / 13 / 31 / 24 / 42 (knobs 3 / 4).
- Channel swap (patterns move, levels stay with the wires).
- Pads connected 1-4 (a channel routed onto an unconnected pad is silent; ramps back over 1 s).
- Pulse shape per channel (see below).
- Box selection (two FOC-Stims; remembered).
- Skip the mode-change ramp (the ET-312's ~3 s ramp on every pattern change; off = faithful).

## Safety (all required)
1. **STOP always wins:** the MX button while running = instant zero, never a toggle back on. Start = arm with the
   slow start (master ramps from 0).
2. **Turn-up rate limit:** levels and master rise at most ~10 steps/s however fast a knob spins; down is instant.
3. **Trips and link loss:** show the fault ("power-cycle the box"), reconnect by itself, come back STOPPED with
   levels at 0. If the M5 dies or Wi-Fi drops, the box's 4 s keepalive stops output on its own.
4. **Levels 0 on boot.**
5. **Caps are set from the PC** (amplitude cap pushed to the M5), not on the remote.
6. The box keeps its own protections: over-current e-stop (margin never widened), v6 peak guard, 0.2 A cap, the
   hardware knob as master.
7. The engine's safety stack (volume law, slow start, deadman, caps) is ported to the M5 and must match the Python
   engine's behaviour (tests).

## Running display
- Like foc312's page (glyph + strip per channel), tweaked for 320x240.
- **All the resistance numbers:** per-electrode output and skin resistance, the pair estimate, plus peak vs command,
  sigma and guard activity.
- Also: pattern name, levels, master, armed/stopped, M5 battery, box battery, link status.

## Link
- **Wi-Fi** to the box's TCP server (:55533, the same HDLC + protobuf stream as USB; the engine already supports
  `--tcp`). Box addresses and Wi-Fi credentials are set from the PC.
- **Direct mode (2026-09-28, in use):** through the house Wi-Fi, Stroke at MA 95 % "couldn't keep up": tick a steady
  17 ms but round trip ~100 ms average, worst ~1 s. Measured causes: the Living-room AP's 2.4 GHz radio (ch 1,
  39 clients) at 78 % channel use and ~69 % retries to the remote, and every update crossing the air twice.
  Load was never the limit (Stroke ~840 B/s, Waves ~4 KB/s against the box's ~10 KB/s ESP32->STM32 UART; both ends
  already run without power save and with Nagle off). Now the remote is the access point (`[direct]` in
  config/m5.toml, ch 6, HT20) and each box joins it once over USB (`stimengine.remote pair-box`; `--house` undoes it).
  Result, same test: **rtt 33 ms average / 80 ms worst**, PlaStim: "works well". The rtt includes up to one 16 ms
  control tick of waiting to read the ack, so the box acts on an update roughly 10-15 ms after it is sent.
- **Remote on first:** the box's ESP32 firmware (diglet48/FOC-Stim-esp32) retries a network twice, then stops until
  power-cycled. Next step if needed: ESP-NOW via a fork of that firmware (encrypted, paired over USB), which also
  lifts the retry limit.

## Patterns
- The PC software compiles patterns (our routines, ErosLink routines, and the ET-312 built-in modes from the user's
  own firmware data) into small bytecode files and loads them onto the M5 (LittleFS). The M5 never parses .elk files.
- The ErosTek data rule still holds: built-in modes come from the user's own firmware image, never from us.

## Pulse shapes (PlaStim: made a big difference in testing; consider more)
- Now: Rounded (half-sine), Square (bench only), Soft square (20 us edges).
- Candidates for a firmware v7: raised-cosine (sin^2), triangle, trapezoid with an adjustable edge time,
  fast-rise/slow-fall (exponential-like), and a continuous "shape" parameter (edge fraction 0 = square ... 0.5 =
  triangle, edge curve linear/cosine) so shapes can be explored by knob rather than from a fixed list.
- Every new shape: exact charge balance (as v2), a 1 kOhm check, then skin at low level; the peak guard covers
  sharper edges.

## Build order (proposed)
1. C++ core (ET-312 VM + mode logic + mapping to the fork's channels + safety stack), verified tick-by-tick against
   golden traces recorded from the Python engine (host build, no hardware).
2. PC side: pattern compiler/exporter and the M5 loading protocol; caps and box list pushed from the PC.
3. M5 firmware: Wi-Fi link to the box, inputs, screens (running / pattern / options), trips and reconnect.
4. Pulse shapes v7 in the fork firmware (+ host + M5).
5. Display tuning against the foc312 page.

## Timing decisions (PlaStim, 2026-09-28)
- Axis glide 17 ms (one 60 Hz tick; was 30 ms). Pulse-shape fade 20 ms out + 40 ms in (was 0.15 + 0.30 s, from
  before the v6 peak guard). Both in the Python engine and the C core, identical.
