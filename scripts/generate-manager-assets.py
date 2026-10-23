#!/usr/bin/env python3
"""Generate lossless LVGL v9 image assets from the existing Manager PNGs.

Uses only Python's standard library. The pinned sources are noninterlaced RGBA8
PNG files. Output is straight-alpha BGRA (I8 when its palette is lossless), with a
12-byte LVGL v9 header and a
4 KiB-window LZSS stream wrapped in binary base64 sextets (0x80..0xBF), decoded
only when an image is first installed. This alphabet keeps encoded data words
outside the target firmware address ranges without exempting data from scans.
Run --check in CI to verify pixels, stream round trips and generated-file drift.
"""
import argparse
from collections import defaultdict, deque
import hashlib
from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
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


def compress(data):
    # A tag below 128 starts tag+1 literal bytes. A tag >=128 starts a
    # (tag&127)+3-byte match followed by little-endian (distance-1).
    positions = defaultdict(deque)
    out, literals, cursor = bytearray(), bytearray(), 0

    def flush():
        if literals:
            out.append(len(literals) - 1)
            out.extend(literals)
            literals.clear()

    while cursor < len(data):
        key = data[cursor:cursor + 3]
        candidates = positions[key]
        while candidates and cursor - candidates[0] > 4096:
            candidates.popleft()
        length, distance = 0, 0
        for previous in reversed(candidates):
            candidate_length = 3
            while candidate_length < 130 and cursor + candidate_length < len(data) and data[previous + candidate_length] == data[cursor + candidate_length]:
                candidate_length += 1
            if candidate_length > length:
                length, distance = candidate_length, cursor - previous
                if length == 130:
                    break
        if length >= 4 and len(key) == 3:
            flush()
            out.append(128 | (length - 3))
            out.extend(struct.pack('<H', distance - 1))
            count = length
        else:
            literals.append(data[cursor])
            if len(literals) == 128:
                flush()
            count = 1
        for position in range(cursor, cursor + count):
            if position + 3 <= len(data):
                queue = positions[data[position:position + 3]]
                while queue and position - queue[0] > 4096:
                    queue.popleft()
                queue.append(position)
        cursor += count
    flush()
    return bytes(out)


def decompress(data, expected):
    result, cursor = bytearray(), 0
    while len(result) < expected:
        tag = data[cursor]
        cursor += 1
        if tag < 128:
            length = tag + 1
            result.extend(data[cursor:cursor + length])
            cursor += length
        else:
            distance, = struct.unpack_from('<H', data, cursor)
            cursor += 2
            distance += 1
            length = (tag & 127) + 3
            if distance > len(result) or len(result) + length > expected:
                raise ValueError('invalid LZSS match')
            for _ in range(length):
                result.append(result[-distance])
    if cursor != len(data) or len(result) != expected:
        raise ValueError('incorrect LZSS stream length')
    return bytes(result)


def encode_storage(data):
    # Three little-endian bytes become four tagged 6-bit values. No data byte
    # can resemble the high byte of a current firmware/RAM address.
    out = bytearray()
    for cursor in range(0, len(data), 3):
        word = int.from_bytes(data[cursor:cursor + 3].ljust(3, b'\0'), 'little')
        out.extend(0x80 | ((word >> shift) & 63) for shift in (0, 6, 12, 18))
    return bytes(out)


def decode_storage(data, size):
    if len(data) != ((size + 2) // 3) * 4:
        raise ValueError('incorrect sextet stream size')
    out = bytearray()
    for cursor in range(0, len(data), 4):
        word = 0
        for index, digit in enumerate(data[cursor:cursor + 4]):
            if not 0x80 <= digit <= 0xBF:
                raise ValueError('incorrect sextet alphabet')
            word |= (digit & 63) << (index * 6)
        out.extend(word.to_bytes(3, 'little'))
    if any(out[size:]):
        raise ValueError('nonzero sextet padding')
    return bytes(out[:size])


def generate():
    lines = ['/* Generated by scripts/generate-manager-assets.py; do not edit. */']
    records, raw_total, stored_total = [], 0, 0
    for index, (name, source) in enumerate(ASSETS):
        path = ROOT / 'manager/src/common' / source
        width, height, rgba = read_png(path)
        image = to_lvgl(width, height, rgba)
        packed = compress(image)
        stored = encode_storage(packed)
        if decode_storage(stored, len(packed)) != packed:
            raise ValueError(f'{name}: sextet round trip failed')
        # Even unaligned words stay above 0x80000000. The verifier scans
        # aligned LE32 words; checking every byte position is stricter.
        if any(int.from_bytes(stored[i:i + 4], 'little') < 0x80000000
               for i in range(len(stored) - 3)):
            raise ValueError(f'{name}: unsafe stored word')
        restored = decompress(decode_storage(stored, len(packed)), len(image))
        if restored != image:
            raise ValueError(f'{name}: compression round trip failed')
        verify_image(restored, width, height, rgba)
        # Alphabetic nibbles also keep ASCII hash words outside 0x34xx scratch.
        digest = hashlib.sha256(image).hexdigest()[:12].translate(
            str.maketrans('0123456789abcdef', 'ABCDEFGHIJKLMNOP'))
        lines.append(f'/* {source}: {width}x{height}, straight-alpha BGRA. */')
        lines.append(f'static const unsigned char asset_{index}[] = {{')
        for offset in range(0, len(stored), 20):
            lines.append('    ' + ','.join(f'0x{b:02x}' for b in stored[offset:offset + 20]) + ',')
        lines.append('};')
        records.append(f'    {{"{name}-{digest}.bin", asset_{index}, {len(packed)}u, {len(image)}u}},')
        raw_total += len(image)
        stored_total += len(stored)
    lines.extend(['static const struct asset_record asset_records[RH_MANAGER_ASSET_COUNT] = {', *records, '};', ''])
    return '\n'.join(lines), raw_total, stored_total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    content, raw_total, stored_total = generate()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != content:
            print(f'{OUTPUT.relative_to(ROOT)} is stale; run {Path(__file__).name}', file=sys.stderr)
            return 1
    else:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        OUTPUT.write_text(content)
    print(f'{len(ASSETS)} assets verified: {raw_total:,} image bytes; {stored_total:,} encoded embedded bytes; 4,096-byte decode window')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
