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

    def test_exact_file_mapping_does_not_match_siblings(self):
        config = (f'/resource/font/MiSans-Regular-All.ttf\t'
                  f'{THEME_ROOT}font-generation-g1/FusionPixel.ttf\n').encode()
        result = self.run_config(config, '/resource/font/MiSans-Regular-All.ttf')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f'Mapped path: {THEME_ROOT}font-generation-g1/FusionPixel.ttf', result.stdout)
        sibling = self.run_config(config, '/resource/font/MiSans-Regular-All.ttf.backup')
        self.assertEqual(sibling.returncode, 0, sibling.stderr)
        self.assertIn('No matching rule', sibling.stdout)

    def test_all_firmware_font_files_can_share_one_generation_target(self):
        names = [
            'MiSansF-Semibold.ttf', 'MiSansF-Medium.ttf', 'MiSansF-Demibold.ttf',
            'MiSans-Semibold.ttf', 'MiSans-Regular-All.ttf', 'MiSans-Medium.ttf',
            'MiSans-Medium-All.ttf', 'MiSans-Demibold.ttf', 'MiSans-Demibold-All.ttf'
        ]
        target = f'{THEME_ROOT}font-generations/g1/FusionPixel.ttf'
        config = ''.join(f'/resource/font/{name}\t{target}\n' for name in names).encode()
        for name in names:
            with self.subTest(name=name):
                result = self.run_config(config, f'/resource/font/{name}')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f'Mapped path: {target}', result.stdout)

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
