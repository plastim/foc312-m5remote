# remote/ — the M5 remote's portable core (C99)

foc312 running on the OSSM M5 Remote (spec: `notes/m5-remote.md`). `core/` is plain C99 with no allocation, shared
by the M5 firmware and the host tests:

- `pyrand.{h,c}` — Python-compatible random numbers (CPython's MT19937, seeding, getrandbits, randint).
- `et312.{h,c}` — the ET-312B mode engine: VM (program-block bytecode, modulators, gate, block timers), mode
  selection (incl. Random1), per-channel outputs. A port of `stimengine/et312/{vm,modes,engine}.py`.

The Python engine is the reference. `tests/test_remote_core.py` builds `test/golden_runner.c` with Zig
(`pip install ziglang`) and runs identical scripts through both; every tick must match (the VM's whole memory
and every output, floats bit for bit). The ET-312 built-in modes' blocks are the user's own data (fwdata.py) and
are handed to the core at runtime; they are not in this source.

## Loading the remote (PC side: `stimengine/remote/`)

- `pack.{h,c}` reads the pattern pack (`patterns.bin`, format in `stimengine/remote/pack.py`): CRC-32, every length,
  offset, module number and bytecode op are validated before anything plays; entries point into the file buffer.
- `loader.{h,c}` is the remote's side of the USB loader (protocol in `loader.h`): checksummed 1 KB chunks, a
  temporary file that only replaces the old one if the CRC matches, `ERR busy` while armed, stalled transfers abort.
- `py -3.13 -m stimengine.remote build | load --port COMx | list --port COMx` builds `patterns.bin` (built-in modes
  from the user's own ET-312 data, ErosLink routines, `routines/`) and `config.json` (caps and safety timings from
  `config/engine.toml`; Wi-Fi and boxes from `config/m5.toml`, see `config/m5.example.toml`) and loads them.
- Tests: `tests/test_remote_pack.py` (C reader vs Python builder, damage rejected, entries play identically to the
  Python engine) and `tests/test_remote_loader.py` (the PC loader against `test/device_sim.c`, the real loader + pack
  reader behind a pipe).
