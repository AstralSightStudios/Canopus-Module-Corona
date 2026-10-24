#!/usr/bin/env python3
"""Generate lossless LVGL v9 image resources from the existing Manager PNGs.

Uses only Python's standard library. The pinned sources are noninterlaced RGBA8
PNG files. Each image becomes a standalone LVGL v9 ``.bin`` written under
``build/manager-resources/`` and shipped as a ``.canopus`` package resource
(``resources/common/<name>.bin``). At install the package extractor drops every
resource into the module data root, so the module reads them by name and never
carries image bytes inside the ELF.

``src/generated/manager_assets.inc`` holds only the file names and byte lengths
used at runtime to locate and sanity-check each extracted resource.

Run --check in CI to verify pixels and generated-file drift.
"""
import argparse
from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
RESOURCE_DIR = ROOT / 'build/manager-resources'
OUTPUT = ROOT / 'src/generated/manager_assets.inc'
HEADER = struct.Struct('<BBHHHHH')
ASSETS = [
    ('status-running', 'images/status-running.png'),
    ('status-config-error', 'images/status-config-error.png'),
    ('status-unresponsive', 'images/status-unresponsive.png'),
    ('reload', 'images/reload.png'),
    ('reload-wide', 'images/reload-wide.png'),
    ('del', 'images/del.png'),
    ('del-wide', 'images/del-wide.png'),
    ('none', 'images/none.png'),
    ('check', 'images/true.png'),
    ('uncheck', 'images/false.png'),
    ('connect', 'images/connect.png'),
    ('choose', 'images/choose.png'),
    ('download', 'images/download.png'),
    ('error', 'images/error.png'),
    ('succ', 'images/succ.png'),
    ('logo', 'logo.png'),
]


def paeth(a, b, c):
    p = a + b - c
    distances = (abs(p - a), abs(p - b), abs(p - c))
    return (a, b, c)[distances.index(min(distances))]


def read_png(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not PNG')
    cursor, compressed, dimensions = 8, bytearray(), None
    while cursor < len(data):
        length, = struct.unpack_from('>I', data, cursor)
        kind = data[cursor + 4:cursor + 8]
        body = data[cursor + 8:cursor + 8 + length]
        expected, = struct.unpack_from('>I', data, cursor + 8 + length)
        if zlib.crc32(kind + body) != expected:
            raise ValueError(f'{path}: PNG CRC mismatch')
        if kind == b'IHDR':
            width, height, depth, color, method, filtering, interlace = struct.unpack('>IIBBBBB', body)
            if (depth, color, method, filtering, interlace) != (8, 6, 0, 0, 0):
                raise ValueError(f'{path}: expected noninterlaced RGBA8')
            dimensions = width, height
        elif kind == b'IDAT':
            compressed.extend(body)
        elif kind == b'IEND':
            break
        cursor += length + 12
    if dimensions is None:
        raise ValueError(f'{path}: missing PNG header')
    width, height = dimensions
    stride = width * 4
    raw = zlib.decompress(compressed)
    if len(raw) != height * (stride + 1):
        raise ValueError(f'{path}: invalid PNG scanline length')
    rows, prior = bytearray(), bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        filter_kind = raw[start]
        row = bytearray(raw[start + 1:start + 1 + stride])
        if filter_kind > 4:
            raise ValueError(f'{path}: unknown PNG filter')
        for i in range(stride):
            a = row[i - 4] if i >= 4 else 0
            b = prior[i]
            c = prior[i - 4] if i >= 4 else 0
            prediction = (0, a, b, (a + b) // 2, paeth(a, b, c))[filter_kind]
            row[i] = (row[i] + prediction) & 255
        rows.extend(row)
        prior = row
    return width, height, bytes(rows)


def to_lvgl(width, height, rgba):
    colors = list(dict.fromkeys(zip(rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4])))
    if len(colors) <= 256:
        lookup = {color: index for index, color in enumerate(colors)}
        palette = b''.join(bytes((b, g, r, a)) for r, g, b, a in colors).ljust(1024, b'\0')
        indices = bytes(lookup[pixel] for pixel in zip(rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4]))
        return HEADER.pack(0x19, 0x0a, 0, width, height, width, 0) + palette + indices
    bgra = bytearray(rgba)
    bgra[0::4], bgra[2::4] = rgba[2::4], rgba[0::4]
    return HEADER.pack(0x19, 0x10, 0, width, height, width * 4, 0) + bgra


def verify_image(image, width, height, rgba):
    magic, color, flags, w, h, stride, reserved = HEADER.unpack_from(image)
    if (magic, flags, w, h, reserved) != (0x19, 0, width, height, 0):
        raise ValueError('incorrect LVGL v9 header')
    if color == 0x0a:
        if stride != width or len(image) != 1036 + width * height:
            raise ValueError('incorrect I8 image size')
        pixels = b''.join(image[12 + index * 4:16 + index * 4] for index in image[1036:])
    elif color == 0x10:
        if stride != width * 4 or len(image) != 12 + width * height * 4:
            raise ValueError('incorrect ARGB8888 image size')
        pixels = image[12:]
    else:
        raise ValueError('unexpected LVGL color format')
    if pixels[0::4] != rgba[2::4] or pixels[1::4] != rgba[1::4] or pixels[2::4] != rgba[0::4] or pixels[3::4] != rgba[3::4]:
        raise ValueError('pixel or alpha mismatch')


def build_resources():
    """Return {filename: image_bytes} and the .inc metadata text."""
    records, files, raw_total = [], {}, 0
    for name, source in ASSETS:
        path = ROOT / 'manager/src/common' / source
        width, height, rgba = read_png(path)
        image = to_lvgl(width, height, rgba)
        verify_image(image, width, height, rgba)
        filename = f'{name}.bin'
        files[filename] = image
        records.append(f'    {{"{filename}", {len(image)}u}},')
        raw_total += len(image)
    lines = ['/* Generated by scripts/generate-manager-assets.py; do not edit. */']
    lines.extend(['static const struct asset_record asset_records[RH_MANAGER_ASSET_COUNT] = {', *records, '};', ''])
    return files, '\n'.join(lines), raw_total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    files, content, raw_total = build_resources()
    stale = OUTPUT.exists() and OUTPUT.read_text() != content
    if args.check:
        bad = []
        for filename, image in files.items():
            target = RESOURCE_DIR / filename
            if not target.exists() or target.read_bytes() != image:
                bad.append(str(target.relative_to(ROOT)))
        if stale or bad:
            if stale:
                print(f'{OUTPUT.relative_to(ROOT)} is stale; run {Path(__file__).name}', file=sys.stderr)
            for path in bad:
                print(f'{path} is stale; run {Path(__file__).name}', file=sys.stderr)
            return 1
    else:
        RESOURCE_DIR.mkdir(parents=True, exist_ok=True)
        for filename, image in files.items():
            (RESOURCE_DIR / filename).write_bytes(image)
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        OUTPUT.write_text(content)
    print(f'{len(ASSETS)} resources verified: {raw_total:,} image bytes as .canopus package resources')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
