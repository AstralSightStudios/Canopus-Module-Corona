"""Drive the actual production bounded ZIP extractor using independent zlib/ZIP fixtures."""
import io
import os
from pathlib import Path
import random
import stat
import struct
import subprocess
import sys
import tempfile
import warnings
import zipfile
import zlib

BINARY = Path(sys.argv[1]).resolve()
MANIFEST = b'{"themeId":"test","name":"Test"}'
COUNT = 0


def packed(entries, compression=zipfile.ZIP_DEFLATED, descriptor=False, comment=b""):
    class Forward(io.BytesIO):
        def seekable(self):
            return False

        def seek(self, *args):
            raise io.UnsupportedOperation()

    stream = Forward() if descriptor else io.BytesIO()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        with zipfile.ZipFile(stream, "w", compression=compression) as z:
            z.comment = comment
            for name, contents in entries:
                z.writestr(name, contents)
    return stream.getvalue()


def locations(data):
    end = data.rfind(b"PK\x05\x06")
    cd = struct.unpack_from("<I", data, end + 16)[0]
    result = []
    at = cd
    while data[at:at+4] == b"PK\x01\x02":
        name, extra, comment = struct.unpack_from("<HHH", data, at + 28)
        local = struct.unpack_from("<I", data, at + 42)[0]
        result.append((at, local))
        at += 46 + name + extra + comment
    return result


def mutate(data, offset, fmt, value):
    changed = bytearray(data)
    struct.pack_into(fmt, changed, offset, value)
    return bytes(changed)


def raw_entry(compressed, contents, method=8):
    """Single-entry ZIP independent of zipfile, for raw RFC 1951 bitstreams."""
    name = b"corona.json"
    crc = zlib.crc32(contents)
    local = struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, 0, method, 0, 0, crc,
                        len(compressed), len(contents), len(name), 0) + name + compressed
    cd = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 20, 20, 0, method,
                     0, 0, crc, len(compressed), len(contents), len(name), 0, 0,
                     0, 0, 0, 0) + name
    return local + cd + struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 1, 1, len(cd), len(local), 0)


def check(data, expected=1, mode=0, existing=False, expected_files=None):
    global COUNT
    COUNT += 1
    with tempfile.TemporaryDirectory(prefix="corona-zip-") as work:
        root = Path(work)
        archive, staging = root / "upload.zip", root / "stage"
        archive.write_bytes(data)
        if existing:
            staging.mkdir()
            (staging / "must-survive").write_text("owned")
        result = subprocess.run([str(BINARY), str(archive), str(staging), str(expected), str(mode)],
                                text=True, capture_output=True)
        if result.returncode:
            raise AssertionError(f"fixture {COUNT}: {result.stdout}\n{result.stderr}")
        if expected == 1:
            if expected_files is not None:
                for name, contents in expected_files:
                    dest = staging / name
                    assert dest.is_dir() if name.endswith("/") else dest.read_bytes() == contents
            else:
                with zipfile.ZipFile(archive) as z:
                    for item in z.infolist():
                        dest = staging / item.filename
                        if item.is_dir():
                            assert dest.is_dir()
                        else:
                            assert dest.read_bytes() == z.read(item), item.filename
        if expected == -8 and staging.exists():
            for dest in staging.rglob("*"):
                assert len(dest.name.encode("utf-8")) <= 63, dest
        if mode in (6, 7):
            assert not staging.exists(), "size failure created staging"
        if existing:
            assert (staging / "must-survive").read_text() == "owned"


rng = random.Random(11951)
entries = [("icons/", b""), ("icons/a.bin", bytes(range(256)) * 140),
           ("icons/中文.bin", b"image"), ("corona.json", MANIFEST)]
for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
    for descriptor in (False, True):
        data = packed(entries, method, descriptor)
        check(data)
        check(data, mode=1)
check(packed(entries), mode=5)
check(packed(entries, comment=b"comment" * 9000))
check(packed([("canora.json", MANIFEST)]))
check(packed([("corona.json", b"")]))
check(packed(entries), -7, existing=True)
for mode in (2, 3, 4, 6):
    check(packed(entries), -5, mode=mode)
check(packed(entries), -7, mode=7)
# A comment can legally contain EOCD-looking bytes. Python's own ZipFile
# scanner misreads these comments too, so validate output against the original
# fixture contents rather than using ZipFile as the oracle for these cases.
check(packed(entries, comment=b"PK\x05\x06" + bytes(18)), expected_files=entries)
check(packed(entries, comment=bytes(1024) + b"PK\x05\x06" + bytes(18)), expected_files=entries)
# Other impossible candidates also permit the scan to reach the true EOCD.
for disk, disk_entries, entries_count, size, offset in (
    (1, 1, 1, 47, 0),
    (0, 2, 1, 47, 0),
    (0, 65535, 65535, 47, 0),
    (0, 1, 1, 0, 0xffffffff),
):
    comment = struct.pack("<IHHHHIIH", 0x06054b50, disk, 0, disk_entries,
                          entries_count, size, offset, 0)
    check(packed(entries, comment=comment), expected_files=entries)
check(bytes(22), -7)

# zlib emits fixed, dynamic, stored blocks and uses the complete 32 KiB history.
for size in (0, 1, 31, 511, 512, 513, 4096, 32768, 65536):
    for level, strategy in ((0, zlib.Z_DEFAULT_STRATEGY), (6, zlib.Z_DEFAULT_STRATEGY),
                            (9, zlib.Z_FIXED), (6, zlib.Z_HUFFMAN_ONLY)):
        content = (bytes(rng.randrange(256) for _ in range(1000)) * 70)[:size]
        encoder = zlib.compressobj(level=level, wbits=-15, strategy=strategy)
        stream = encoder.compress(content) + encoder.flush()
        check(raw_entry(stream, content))
# Consecutive sync-flushed blocks exercise bit alignment and zero-output steps.
encoder = zlib.compressobj(wbits=-15)
body = b"hello" * 4000
stream = encoder.compress(body[:10000]) + encoder.flush(zlib.Z_SYNC_FLUSH)
stream += encoder.compress(body[10000:]) + encoder.flush()
check(raw_entry(stream, body))

for path in ("../outside", "a/../../outside", "/absolute", "a//b", "a/./b", "a/../b",
             "a\\b", "C:/x", "a/\x01b", "a/\x7fb", "mappings.tsv", "nested/mappings.tsv"):
    check(packed([("corona.json", MANIFEST), (path, b"evil")]), -7)
for sequence in (
    [("corona.json", MANIFEST), ("corona.json", MANIFEST)],
    [("corona.json", MANIFEST), ("canora.json", MANIFEST)],
    [("corona.json", MANIFEST), ("a", b"file"), ("a/b", b"child")],
    [("corona.json", MANIFEST), ("a/b", b"child"), ("a", b"file")],
    [("corona.json", MANIFEST), ("a/", b""), ("a/", b"")],
    [("corona.json", MANIFEST), ("a/", b""), ("a", b"")],
    [("a.bin", b"no manifest")],
):
    check(packed(sequence), -7)
check(packed([("corona.json", b"x" * 65537)]), -8)
check(packed([("corona.json", MANIFEST), ("x" * 250, b"x")]), -8)
# Names are limited by the SDK's 64-byte dirent buffer, including its NUL.
# Exercise both leaves and directory components, measured in UTF-8 bytes.
for component in ("x" * 63, "中" * 21):
    check(packed([("corona.json", MANIFEST), (component, b"leaf")]))
    check(packed([("corona.json", MANIFEST), (component + "/", b""),
                  (component + "/x.bin", b"child")]))
for component in ("x" * 64, "中" * 21 + "x"):
    check(packed([("corona.json", MANIFEST), (component, b"leaf")]), -8)
    check(packed([("corona.json", MANIFEST), (component + "/", b"")]), -8)
    check(packed([("corona.json", MANIFEST), (component + "/x.bin", b"child")]), -8)

# Reordered central directory remains a normal, accepted ZIP.
data = packed(entries)
records = locations(data)
cd_at = records[0][0]
eocd = data.rfind(b"PK\x05\x06")
chunks = [data[at:(records[i+1][0] if i+1 < len(records) else eocd)]
          for i, (at, _) in enumerate(records)]
check(data[:cd_at] + b"".join(reversed(chunks)) + data[eocd:])

# Signed and unsigned data descriptors, including mismatch and truncated streams.
data = packed([("corona.json", MANIFEST)], descriptor=True)
cd, local = locations(data)[0]
descriptor = cd - 16
assert data[descriptor:descriptor+4] == b"PK\x07\x08"
unsigned = data[:descriptor] + data[descriptor+4:]
unsigned = mutate(unsigned, len(unsigned)-22+16, "<I", cd-4)
check(unsigned)
check(mutate(data, descriptor+8, "<I", 1), -7)
check(mutate(data, local+6, "<H", 0), -7)

base = packed([("corona.json", MANIFEST), ("data.bin", b"A" * 5000)])
cd, local = locations(base)[0]
for at, fmt, value in (
    (cd+8, "<H", 1),                 # encrypted
    (cd+8, "<H", 64),                # strong encryption
    (cd+10, "<H", 12),               # bzip2
    (cd+6, "<H", 45),                # ZIP64 version
    (cd+34, "<H", 1),                # another disk
    (cd+24, "<I", 0xffffffff),       # ZIP64 sentinel
    (cd+24, "<I", 64*1024*1024+1),
    (cd+42, "<I", cd),               # local header in central directory
    (cd+38, "<I", (stat.S_IFLNK|0o777)<<16),
    (cd+38, "<I", (stat.S_IFCHR|0o600)<<16),
    (cd+38, "<I", 8),                # DOS volume label
    (local+8, "<H", 0),              # method disagrees
    (local+4, "<H", 10),             # version disagrees
    (local+10, "<I", 1),             # timestamp disagrees
    (local+14, "<I", 0),             # CRC disagrees
):
    check(mutate(base, at, fmt, value), -8 if at == cd+24 and value == 64*1024*1024+1 else -7)
# Both headers lie about CRC: extraction still checks the actual bytes.
bad = mutate(base, cd+16, "<I", 1234)
bad = mutate(bad, local+14, "<I", 1234)
check(bad, -7)
# Reject unconsumed compressed bytes, invalid block types, truncated streams.
encoder = zlib.compressobj(wbits=-15)
stream = encoder.compress(MANIFEST) + encoder.flush()
for invalid in (stream+b"junk", stream[:-1], b"\x07", b"\x01\x01\x00\x00\x00x"):
    check(raw_entry(invalid, MANIFEST), -7)
# Declared output differs from actual output.
check(raw_entry(stream, MANIFEST+b"x"), -7)
check(raw_entry(stream, MANIFEST[:-1]), -7)
# Gaps/extra local bytes and directory trailing data are rejected.
check(base[:-22] + b"junk" + base[-22:], -7)
check(base[:local+30] + b"x" + base[local+30:], -7)
# Invalid central UTF-8 and NUL bytes cannot become filesystem paths.
name_at = cd+46
check(mutate(base, name_at, "<B", 0), -7)
check(mutate(base, name_at, "<B", 255), -7)

# Malformed extra records and ZIP64 extras, using zipfile's extra-field writer.
for extra in (b"\x01\x00\x00\x00", b"\x02\x00\x10\x00", b"\x01"):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as z:
        item = zipfile.ZipInfo("corona.json")
        item.extra = extra
        z.writestr(item, MANIFEST)
    check(stream.getvalue(), -7)

# Long history and many blocks are compared byte-for-byte against zipfile.
random_block = bytes(rng.randrange(256) for _ in range(32768))
check(packed([("corona.json", MANIFEST), ("long.bin", random_block * 8)]))
for _ in range(96):
    invalid = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 100)))
    try:
        zlib.decompress(invalid, wbits=-15)
    except zlib.error:
        check(raw_entry(invalid, MANIFEST), -7)

print(f"corona archive: {COUNT} fixture/fault cases passed (ASan/UBSan harness)")
