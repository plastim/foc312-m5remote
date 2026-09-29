# PlaStim foc312 M5 remote

A handheld controller for **FOC-Stim** boxes running the [foc312](https://github.com/plastim/foc312) firmware. It
plays the same ET-312-style patterns as the [PlaStim foc312 engine](https://github.com/plastim/foc312-engine),
with no computer needed, straight to the box over Wi-Fi.

It runs on the **OSSM M5 Remote** hardware by ortlof: an M5Stack CoreS3 SE on a carrier with four encoders and a
button. Build or buy one from **[ortlof/OSSM-M5-Remote](https://github.com/ortlof/OSSM-M5-Remote)**; this repository
is its firmware for e-stim (written from scratch; the hardware design is ortlof's).

## Controls

| Control | Main screen | Pattern list | Options (wiring picture) |
|---|---|---|---|
| Knob 1 turn | master volume | scroll | move the highlight |
| Knob 1 press | open patterns | pick | toggle / next |
| Knob 2 turn | MA (the ET-312's sensation knob) | jump 10 | pulse shape |
| Knob 3 / 4 turn | level of each wire pair | - | that pair's wires, both directions |
| Knob 4 press | options | back | back |
| MX button | start / **STOP** | start / STOP | start / STOP |
| Power button | short press: switch off (output stopped first) | | |

In the menus no knob touches the output, and STOP always works. The box's own knob stays the master limit: the
remote's master is a share of it.

## Setting it up (with the PC app)

1. Flash this firmware from the PC app's hub (**M5 remote** tab), or build it yourself (below).
2. In the same tab, set the Wi-Fi mode and add your box(es), then **Load patterns & settings**. The remote gets the
   patterns and the safety limits set on the PC (current cap, slow start, deadman); they are never set on the remote.
3. Recommended: let the remote run **its own Wi-Fi network** and pair the box to it (one click in the hub). It is a
   direct link, much faster than going through a busy house access point. Switch the remote on before the box.

## Safety

The remote runs the same safety stack as the PC engine: a hard current cap, a slow start on every start, a rate limit
on turning up (turning down is instant), and STOP that always wins. If the Wi-Fi link drops, the box stops by itself
within seconds. If the box trips, the remote shows the box's trip report; power-cycle the box and start from zero.
Read the safety notes in the [engine's README](https://github.com/plastim/foc312-engine#safety).
**This is not a medical device. You use it at your own risk.**

## Building

```
cd m5
pio run -e app -t upload --upload-port COMx
```

`core/` is portable C99 with no allocation (the ET-312 emulator, the safety stack, the box link); it is tested on
the PC against the Python engine by the engine's test suite. `tools/package_m5.py` makes the release image.

## License and credits

[PolyForm Noncommercial 1.0.0](LICENSE.md); third-party parts in `NOTICE`. Hardware:
[ortlof/OSSM-M5-Remote](https://github.com/ortlof/OSSM-M5-Remote) (CC BY-SA 4.0).

Support the project: [Patreon](https://www.patreon.com/plastim) · [PlaStim store](https://plastim.net/)
