#!/usr/bin/env python3
"""Generate disposable Corona UI fixtures; contains no firmware resources.

  python3 tests/fixtures/manager-emulator/generate.py 155 /tmp/corona-seed-155

Forty installed themes exercise virtual rows beyond the 32-node snapshot limit.
The first package is active/protected, the system divider lies immediately after
it. Each image is a generated 16x16 LVGL v9 ARGB8888 tile. The real calendar
resource path is used only as a preview/redirect key, never copied from firmware.
The remaining paths are isolated synthetic keys that no firmware page consumes.
"""
import argparse
import json
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=("043", "155"))
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        parser.error("output must be empty")
    args.output.mkdir(parents=True, exist_ok=True)
    source = "/resource/app/perpetual_calendar/" + ("calendar_background_icon.bin" if args.target == "043" else "launcher_icon.bin")
    ids = [f"corona_ui_{i:02d}" for i in range(40)]

    def save(name, value):
        path = args.output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")

    save("interconnect-themes.json", ids)
    save("resource-order.json", {"version": 1, "order": [ids[0], "@system", *ids[1:]]})
    save("resource-overrides.json", {"version": 2, "overrides": {source: "@system"}, "features": {"controlCenter": "@blur"}})
    # The active first package makes the existing delete guard observable before
    # any reload. It remains protected even though the mix override prefers ROM.
    (args.output / "mappings.tsv").write_text(f"{source}\tthemes/{ids[0]}/tile.bin\n")
    for i, theme in enumerate(ids):
        mappings = [{"source": source, "destination": "tile.bin"},
                    {"source": f"/system/corona-ui/{i:02d}.bin", "destination": "tile.bin"}]
        manifest = {"format": "canopus-resource-pack", "formatVersion": 1,
                    "themeId": theme, "name": f"验收主题 {i:02d}", "author": "Canopus 测试",
                    "version": "1.0.0", "versionCode": 1, "description": "原生列表、排序与图标预览验收",
                    "targets": ["xiaomi-band-10-pro-3.101.043" if args.target == "043" else "xiaomi-band-11-4.100.155"],
                    "mappings": mappings, "backgrounds": {"controlCenter": {"mode": "image", "image": "tile.bin"}}}
        save(f"themes/{theme}/corona.json", manifest)
        color = ((50 + i * 29) % 220 + 30, (80 + i * 37) % 220 + 30, (100 + i * 41) % 220 + 30)
        r, g, b = color
        header = struct.pack("<BBHHHHH", 0x19, 0x10, 0, 16, 16, 64, 0)
        (args.output / f"themes/{theme}/tile.bin").write_bytes(header + bytes((b, g, r, 255)) * 256)
    print(args.output.resolve())


if __name__ == "__main__":
    main()
