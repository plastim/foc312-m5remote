"""Package the M5 remote's firmware as one image the PC app (or esptool) flashes at 0x0.

    py -3.13 tools/package_m5.py                   (after `pio run -e app` in m5/)

Reads PlatformIO's build (m5/.pio/build/app: bootloader.bin, partitions.bin, firmware.bin) plus the Arduino
core's boot_app0.bin, takes the offsets from the partition table itself (the ESP32-S3 bootloader sits at 0x0, the
table at 0x8000, boot_app0 at the otadata partition, the app at the first app partition), merges them with
`esptool merge-bin` into release/stim-remote-<git short sha>.bin and adds it to release/manifest.json.
The image stops at the end of the app, so flashing it leaves the LittleFS partition (patterns, settings) alone.
"""
from __future__ import annotations

import hashlib
import json
import struct
import subprocess
import sys
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]          # the foc312-m5remote project
BUILD = ROOT / "m5" / ".pio" / "build" / "app"
BOOT_APP0 = Path.home() / ".platformio" / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / \
    "boot_app0.bin"
RELEASE = ROOT / "release"
BOOTLOADER_OFFSET = 0x0          # ESP32-S3 (0x1000 on the original ESP32)
PARTITIONS_OFFSET = 0x8000


def partitions(table: bytes) -> list[tuple[int, int, int, int, str]]:
    """(type, subtype, offset, size, label) per entry of an ESP-IDF partition table."""
    out = []
    for i in range(0, len(table), 32):
        e = table[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        off, size = struct.unpack("<II", e[4:12])
        out.append((e[2], e[3], off, size, e[12:28].rstrip(b"\0").decode("ascii", "replace")))
    return out


def main() -> int:
    table = (BUILD / "partitions.bin").read_bytes()
    parts = partitions(table)
    app = next(p for p in parts if p[0] == 0)                       # first app partition (app0 / factory)
    otadata = next((p for p in parts if p[0] == 1 and p[1] == 0), None)
    sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True,
                         check=True).stdout.strip()
    RELEASE.mkdir(parents=True, exist_ok=True)
    out = RELEASE / f"stim-remote-{sha}.bin"
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge-bin", "-o", str(out),
           hex(BOOTLOADER_OFFSET), str(BUILD / "bootloader.bin"), hex(PARTITIONS_OFFSET), str(BUILD / "partitions.bin")]
    if otadata is not None:
        cmd += [hex(otadata[2]), str(BOOT_APP0)]
    cmd += [hex(app[2]), str(BUILD / "firmware.bin")]
    subprocess.run(cmd, check=True)
    digest = hashlib.sha256(out.read_bytes()).hexdigest()
    manifest = RELEASE / "manifest.json"
    images = json.loads(manifest.read_text(encoding="utf-8")).get("images", []) if manifest.exists() else []
    images = [im for im in images if im.get("file") != out.name]
    for im in images:
        im["recommended"] = False
    images.insert(0, {"id": f"remote-{sha}", "name": f"stim remote ({sha})", "version": sha, "file": out.name,
                      "sha256": digest, "recommended": True,
                      "notes": f"built {date.today().isoformat()} from commit {sha}; offsets bootloader 0x0, "
                               f"partitions 0x8000, boot_app0 {hex(otadata[2]) if otadata else '-'}, "
                               f"app {hex(app[2])}. Patterns and settings (LittleFS) are kept."})
    manifest.write_text(json.dumps({"images": images}, indent=1) + "\n", encoding="utf-8")
    print(f"{out.name}: {out.stat().st_size} bytes, sha256 {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
