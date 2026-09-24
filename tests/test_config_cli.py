import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
THEME_ROOT = '/data/quickapp/files/ng.lst.corona/themes/'


class ConfigCLI(unittest.TestCase):
    def run_config(self, data, path=None):
        with tempfile.TemporaryDirectory() as directory:
            config = pathlib.Path(directory) / 'mappings.tsv'
            config.write_bytes(data)
            args = [str(ROOT / 'build/check-config'), str(config)]
            if path is not None:
                args.append(path)
            return subprocess.run(args, capture_output=True, text=True)

    def test_example_and_preview(self):
        result = self.run_config((ROOT / 'examples/mappings.tsv').read_bytes(), '/resource/icons/a.bin')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f'Mapped path: {THEME_ROOT}current/icons/a.bin', result.stdout)

    def test_unmatched_path_is_explicit(self):
        result = self.run_config(f'/resource/\t{THEME_ROOT}current/\n'.encode(), '/other/a.bin')
        self.assertEqual(result.returncode, 0)
        self.assertIn('No matching rule', result.stdout)

    def test_empty_and_malformed_config(self):
        for data in (b'', b'# comment\n', f'/resource/\\t{THEME_ROOT}current/\n'.encode(),
                     f'/resource/\t{THEME_ROOT}../outside/\n'.encode(), b'x' * 32769):
            with self.subTest(data=data[:50]):
                self.assertNotEqual(self.run_config(data).returncode, 0)

    def test_invalid_resource_path(self):
        self.assertNotEqual(self.run_config(f'/resource/\t{THEME_ROOT}current/\n'.encode(),
                                           '/resource/../x').returncode, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
