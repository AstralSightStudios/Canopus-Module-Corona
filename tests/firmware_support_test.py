"""Host-only checks for firmware target routing; no emulator/build is needed."""
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

HERE = Path(__file__).resolve().parent
SOURCE = 'xiaomi-band-11-4.100.139'
PORT = 'xiaomi-band-11-4.100.155'


def support(target=None, **environment):
    env = dict(os.environ)
    env.pop('RESOURCE_HOOK_TARGET', None)
    env.pop('CANOPUS_ROOT', None)
    if target is not None:
        env['RESOURCE_HOOK_TARGET'] = target
    env.update(environment)
    spec = importlib.util.spec_from_file_location('firmware_support_fixture', HERE / 'firmware_support.py')
    module = importlib.util.module_from_spec(spec)
    with patch.dict(os.environ, env, clear=True), patch.object(sys, 'path', sys.path.copy()):
        spec.loader.exec_module(module)
    return module


class TargetRouting(unittest.TestCase):
    def test_default_and_explicit_target(self):
        self.assertEqual(support().TARGET, SOURCE)
        self.assertEqual(support(PORT).TARGET, PORT)
        with self.assertRaisesRegex(RuntimeError, 'unsupported RESOURCE_HOOK_TARGET'):
            support('xiaomi-band-11-4.100.999')

    def test_framework_override_and_default_fallback(self):
        self.assertEqual(support(CANOPUS_ROOT='/tmp/framework-fixture').CANOPUS,
                         Path('/tmp/framework-fixture').resolve())
        m = support()
        expected = m.ROOT.parent / 'Canopus'
        if not expected.is_dir():
            expected = m.ROOT.parent / 'Canopus-Private'
        self.assertEqual(m.CANOPUS, expected.resolve())

    def test_exact_mappings_and_unknown_fail_closed(self):
        m = support(PORT)
        self.assertEqual(m.fw(0x0c3a6194), 0x0c3a6194)
        self.assertEqual(m.fw(0x0c3a6195), 0x0c3a6195)
        self.assertEqual(m.fw(0x2ca168c4), 0x2ca168b4)
        self.assertEqual(m.fw(0x2ca16944), 0x2ca16934)
        self.assertEqual(m.fw(0x0c8b9790), 0x0c8b9780)
        self.assertEqual(m.fw(0x0c8b9791), 0x0c8b9781)
        self.assertEqual(m.fw(0x0c696e34), 0x0c696e24)
        self.assertEqual(m.fw(0x0c904cfc), 0x0c904cec)
        with self.assertRaisesRegex(RuntimeError, 'unsupported firmware test address'):
            m.fw(0x0c000002)
        with self.assertRaisesRegex(RuntimeError, 'unsupported firmware test address'):
            m.fw(0x200bd3b9)  # a byte global is not a Thumb function pointer
        with self.assertRaisesRegex(RuntimeError, 'explicitly translate'):
            m.require_identity_addresses(0x2ca168c4)
        self.assertEqual(support().fw(0x0c000002), 0x0c000002)

    def test_machine_always_forwards_selected_target(self):
        for target in (SOURCE, PORT):
            m = support(target)
            factory = Mock()
            with patch.object(m, 'check_firmware'), patch.dict(
                    sys.modules, {'band11_arm_bootstrap': types.SimpleNamespace(Machine=factory)}):
                m.Machine(fault='nomem')
                factory.assert_called_once_with(target=target, fault='nomem')
                with self.assertRaisesRegex(RuntimeError, 'differs from'):
                    m.Machine(target='another-target')

    def test_firmware_fingerprint_and_missing_file_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            m = support(PORT, CANOPUS_ROOT=tmp)
            with self.assertRaisesRegex(RuntimeError, 'missing selected-target firmware'):
                m.check_firmware()
            firmware = Path(tmp) / 'fwbins' / PORT / 'vela_ap.bin'
            firmware.parent.mkdir(parents=True)
            firmware.write_bytes(b'wrong firmware')
            with self.assertRaisesRegex(RuntimeError, 'firmware fingerprint mismatch'):
                m.check_firmware()
            m.FIRMWARE_SHA256[PORT] = hashlib.sha256(b'wrong firmware').hexdigest()
            m.check_firmware()

    def test_hook_keeps_source_dictionary_key_but_uses_target_pc(self):
        m = support(PORT)
        # Artificial nonidentity mapping checks the framework's source-keyed
        # dictionary contract without pretending this is a real firmware port.
        source, actual = 0x0c001000, 0x0c002000
        m._ADDRESSES_155[source] = actual
        machine = types.SimpleNamespace(uc=Mock(), firmware_hooks={source: 123},
                                        firmware_call=Mock())
        callback = Mock()
        with patch.dict(sys.modules, {'unicorn': types.SimpleNamespace(UC_HOOK_CODE=4)}):
            m.hook(machine, source, callback)
        machine.uc.hook_del.assert_called_once_with(123)
        machine.uc.hook_add.assert_called_once_with(
            4, machine.firmware_call, callback, actual, actual)
        self.assertEqual(machine.firmware_hooks[source], machine.uc.hook_add.return_value)
        self.assertNotIn(actual, machine.firmware_hooks)

    def test_synthetic_page_callbacks_are_not_firmware_addresses(self):
        m = support(PORT)
        machine = types.SimpleNamespace(uc=Mock(), firmware_hooks={}, firmware_call=Mock())
        callback = Mock()
        address = 0x1c7f0000
        with patch.dict(sys.modules, {'unicorn': types.SimpleNamespace(UC_HOOK_CODE=4)}):
            m.hook(machine, address, callback, synthetic=True)
            machine.uc.hook_add.assert_called_once_with(
                4, machine.firmware_call, callback, address, address)
            with self.assertRaisesRegex(ValueError, 'PSRAM'):
                m.hook(machine, 0x0c900000, callback, synthetic=True)

    def test_unknown_target_fails_before_dependency_import(self):
        for name in ('font_lifecycle', 'font_retarget', 'image_lifecycle', 'page_rebuild',
                     'paths', 'rebind', 'restart', 'ui_redraw'):
            with self.subTest(suite=name):
                env = dict(os.environ, RESOURCE_HOOK_TARGET='unknown-firmware')
                result = subprocess.run([sys.executable, str(HERE / f'firmware_{name}.py')],
                                        env=env, text=True, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('unsupported RESOURCE_HOOK_TARGET', result.stderr)
                self.assertNotIn('No module named', result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
