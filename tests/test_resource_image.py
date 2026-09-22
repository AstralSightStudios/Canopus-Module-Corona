"""Host-only checks; no firmware or device required."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from PIL import Image

spec = importlib.util.spec_from_file_location('resource_image', Path(__file__).resolve().parents[1] / 'tools/resource_image.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def fixture():
    palette = bytearray(1024)
    palette[4:8] = bytes((30, 20, 10, 128))
    palette[8:12] = palette[4:8]
    return module.HEADER.pack(0x19, 10, 0, 2, 1, 3, 0) + palette + bytes((2, 0, 77))


class ImageTests(unittest.TestCase):
    def test_bgra_and_transparency(self):
        self.assertEqual(module.rgba_pixels(module.decode(fixture())), [(10, 20, 30, 128), (0, 0, 0, 0)])

    def test_round_trip_preserves_duplicate_indices_and_padding(self):
        data = fixture()
        self.assertEqual(module.encode(module.decode(data), data), data)

    def test_cli_saved_png_round_trip_and_overwrite_refusal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original, png, encoded = (root / name for name in ('original.bin', 'image.png', 'encoded.bin'))
            original.write_bytes(fixture())
            tool = [sys.executable, str(Path(module.__file__))]
            for command in ([*tool, 'decode', str(original), str(png)],
                            [*tool, 'encode', str(png), str(encoded), '--template', str(original)]):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(encoded.read_bytes(), original.read_bytes())
            result = subprocess.run([*tool, 'decode', str(original), str(original)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(original.read_bytes(), fixture())

    def test_cli_rejects_non_png_without_creating_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, template, output = (root / name for name in ('image.bmp', 'original.bin', 'output.bin'))
            Image.new('RGB', (2, 1)).save(source)
            template.write_bytes(fixture())
            result = subprocess.run([sys.executable, str(Path(module.__file__)), 'encode',
                                     str(source), str(output), '--template', str(template)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Input must be PNG', result.stderr)
            self.assertFalse(output.exists())

    def test_edit_preserves_header_and_padding(self):
        data = fixture()
        image = module.decode(data)
        image.putpixel((0, 0), (120, 23, 54, 17))
        result = module.encode(image, data)
        self.assertEqual(result[:12], data[:12])
        self.assertEqual(result[-1], 77)
        self.assertEqual(module.decode(result).tobytes(), image.tobytes())

    def test_bad_headers_and_size(self):
        data = fixture()
        for invalid in (b'', data[:-1], data + b'X', b'\x00' + data[1:], data[:2] + b'\x01' + data[3:]):
            with self.assertRaises(ValueError):
                module.decode(invalid)
        with self.assertRaises(ValueError):
            module.encode(Image.new('RGBA', (3, 3)), data)

    def test_quantization_requires_opt_in(self):
        data = module.HEADER.pack(0x19, 10, 0, 257, 1, 257, 0) + bytes(1024 + 257)
        image = Image.new('RGBA', (257, 1))
        image.putdata([(i % 256, i // 256, 0, 255) for i in range(257)])
        with self.assertRaises(ValueError):
            module.encode(image, data)
        result = module.encode(image, data, quantize=True)
        self.assertLessEqual(len(set(module.rgba_pixels(module.decode(result)))), 256)

    def test_romfs(self):
        data = bytearray(112)
        data[:8] = b'-rom1fs-'
        struct.pack_into('>I', data, 8, len(data))
        data[16:25] = b'resource\0'
        struct.pack_into('>IIII', data, 32, 1, 64, 0, 0)
        data[48:50] = b'.\0'
        struct.pack_into('>IIII', data, 64, 2, 0, 3, 0)
        data[80:86] = b'a.bin\0'
        data[96:99] = b'abc'
        self.assertEqual(module.extract_romfs(bytes(data), '/resource/a.bin'), b'abc')
        for path in ('/missing', '/../a.bin', 'a.bin'):
            with self.assertRaises(ValueError):
                module.extract_romfs(bytes(data), path)
        struct.pack_into('>I', data, 64, 64 | 2)
        with self.assertRaises(ValueError):
            module.extract_romfs(bytes(data), '/missing')


if __name__ == '__main__':
    unittest.main()
