"""Canonical delivery orchestration with mocked compilation and package subprocesses.

Package input selection and output layout are checked here. Archive encoding is
covered separately by the real CLI integration check.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
BAND139 = 'xiaomi-band-11-4.100.139'
BAND155 = 'xiaomi-band-11-4.100.155'
BAND177 = 'xiaomi-band-11-4.100.177'
P67 = 'xiaomi-band-10-pro-3.101.043'


class CanonicalDelivery(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('canonical_delivery', ROOT / 'scripts/build-delivery.py')
        self.builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.builder)
        self.temporary = tempfile.TemporaryDirectory(prefix='rh-canonical-delivery-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'module'
        self.root.mkdir()
        for name in ('src', 'include', 'scripts', 'tests', 'tools', 'docs', 'examples', 'targets'):
            (self.root / name).mkdir()
        (self.root / 'Canopus.toml').write_text((ROOT / 'Canopus.toml').read_text())
        for name in ('README.md', '.gitignore', 'docs/INSTALL.md', 'docs/FONT_RELOAD.md'):
            (self.root / name).write_text('orchestration fixture\n')
        for target in (BAND139, BAND155, BAND177, P67):
            directory = self.root / 'targets' / target
            directory.mkdir()
            (directory / 'evidence.md').write_text('not device evidence\n')
        self.output = self.root / 'delivery'
        self.commands = []
        self.packaged_targets = []
        self.packaged_artifacts = {}
        self.which_calls = []

    @staticmethod
    def artifact(target):
        return ('opaque fresh ELF marker: ' + target).encode()

    def fake_run(self, command, **kwargs):
        command = [str(argument) for argument in command]
        self.commands.append((command, kwargs.get('env', {})))
        if any(Path(argument).name == 'build-install-payload.sh' for argument in command):
            target, destination = command[-2], Path(command[-1])
            destination.mkdir(parents=True)
            (destination / 'resource-hook.elf').write_bytes(self.artifact(target))
            (destination / 'receipt.bin').write_bytes(b'opaque CMI2 marker')
            staged = self.root / 'build' / target / 'corona.elf'
            staged.parent.mkdir(parents=True, exist_ok=True)
            staged.write_bytes(self.artifact(target))
        elif 'package' in command and '--manifest' in command:
            manifest_path = Path(command[command.index('--manifest') + 1])
            manifest = tomllib.loads(manifest_path.read_text())
            self.assertEqual(manifest['module']['id'], 'corona')
            self.assertEqual(manifest['module']['version_code'], 3)
            self.packaged_targets = manifest['targets']['include']
            for target in self.packaged_targets:
                artifact = manifest_path.parent / 'build' / target / 'corona.elf'
                self.packaged_artifacts[target] = artifact.read_bytes()
                self.assertEqual(artifact.read_bytes(), self.artifact(target))
            package = Path(command[command.index('--output') + 1])
            package.write_text(json.dumps({'mock_package_targets': self.packaged_targets}))
        return subprocess.CompletedProcess(command, 0, stdout='', stderr='')

    def fake_which(self, executable):
        self.which_calls.append(str(executable))
        if 'lua' in str(executable) or 'missing-firmware-python' in str(executable):
            return None
        return str(executable)

    def invoke(self, *arguments, firmware_python='/missing-firmware-python'):
        env = {'CANOPUS_ROOT': str(self.root / 'framework'),
               'CANOPUS_CLI': '/mock/canopus', 'CANOPUS_TEST_LUA': '/missing-lua',
               'FIRMWARE_PYTHON': firmware_python}
        with mock.patch.object(self.builder, 'ROOT', self.root), \
             mock.patch.dict(os.environ, env, clear=True), \
             mock.patch.object(sys, 'argv', ['build-delivery.py', str(self.output), *arguments]), \
             mock.patch.object(self.builder.subprocess, 'run', side_effect=self.fake_run), \
             mock.patch.object(self.builder.shutil, 'which', side_effect=self.fake_which), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.builder.main()

    def native_commands(self):
        return [(command, env) for command, env in self.commands
                if any(Path(argument).name.startswith('firmware_') for argument in command)]

    def test_default_is_155_canopus_without_lua_or_watchface(self):
        self.invoke()
        self.assertEqual(self.packaged_targets, [BAND155])
        self.assertTrue((self.output / 'corona-0.3.0.canopus').is_file())
        self.assertFalse((self.output / 'watchface').exists())
        self.assertFalse(self.native_commands())
        self.assertFalse(any('lua' in executable for executable in self.which_calls))
        for command, _ in self.commands:
            self.assertFalse(any('watchface' in argument or 'installer_prod' in argument
                                 for argument in command), command)
        metadata = json.loads((self.output / 'validation.json').read_text())
        self.assertEqual(metadata['targets'], [BAND155])
        self.assertEqual(metadata['native_firmware_suites'], {})
        self.assertIn('--firmware-tests', metadata['native_firmware_suites_not_run'][BAND155])
        self.assertEqual(metadata['delivery_format'], 'canopus')
        self.assertEqual(metadata['package'], 'corona-0.3.0.canopus')
        self.assertEqual(metadata['transport'], 'wearpacket-l2-cpc2')
        self.assertNotIn('native_install_targets', metadata)
        self.assertIn('  corona-0.3.0.canopus\n', (self.output / 'SHA256SUMS').read_text())
        self.assertTrue(any(command[1:3] == ['package', 'verify'] and
                            Path(command[-1]).name == 'corona-0.3.0.canopus'
                            for command, _ in self.commands))

    def test_aggregate_contains_all_selected_fresh_targets_without_transport_gate(self):
        selected = [BAND139, BAND155, BAND177, P67]
        arguments = [argument for target in selected for argument in ('--target', target)]
        self.invoke(*arguments)
        self.assertEqual(self.packaged_targets, selected)
        self.assertEqual(self.packaged_artifacts,
                         {target: self.artifact(target) for target in selected})
        self.assertTrue((self.output / 'corona-0.3.0.canopus').is_file())
        metadata = json.loads((self.output / 'validation.json').read_text())
        self.assertEqual(metadata['targets'], selected)
        self.assertFalse(self.native_commands())

    def test_unselected_stale_artifact_is_excluded(self):
        stale = self.root / 'build' / BAND139 / 'corona.elf'
        stale.parent.mkdir(parents=True)
        stale.write_bytes(b'stale unrelated artifact')
        self.invoke('--target', BAND155, '--target', BAND177)
        self.assertEqual(self.packaged_targets, [BAND155, BAND177])
        self.assertNotIn(BAND139, self.packaged_artifacts)

    def test_firmware_checks_require_explicit_flag_and_dependencies(self):
        with self.assertRaises(SystemExit) as failure:
            self.invoke('--firmware-tests')
        self.assertEqual(failure.exception.code, 2)
        self.assertFalse(self.commands)
        self.assertFalse(self.output.exists())

    def test_explicit_177_records_missing_harness_without_a_transport_gate(self):
        self.invoke('--target', BAND177, '--firmware-tests')
        self.assertEqual(self.packaged_targets, [BAND177])
        self.assertFalse(self.native_commands())
        metadata = json.loads((self.output / 'validation.json').read_text())
        self.assertEqual(metadata['native_firmware_suites'], {})
        self.assertIn('No exact-target firmware harness', metadata['native_firmware_suites_not_run'][BAND177])

    def test_explicit_firmware_checks_run_exact_target_suites(self):
        self.invoke('--target', BAND155, '--firmware-tests', firmware_python='/mock/firmware-python')
        native = self.native_commands()
        names = {Path(command[1]).stem for command, _ in native}
        expected = {name for name, _ in self.builder.firmware_test_commands(BAND155, '/mock/firmware-python')}
        self.assertTrue(expected <= names)
        for command, env in native:
            self.assertEqual(command[0], '/mock/firmware-python')
            self.assertEqual(env['RESOURCE_HOOK_TARGET'], BAND155)
        metadata = json.loads((self.output / 'validation.json').read_text())
        self.assertEqual(set(metadata['native_firmware_suites'][BAND155]), expected)


if __name__ == '__main__':
    unittest.main(verbosity=2)
