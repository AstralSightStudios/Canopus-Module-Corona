#!/usr/bin/env python3
"""Build a signed, tested Resource Hook integration delivery (never deploys it)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import runpy
import subprocess
import sys
import tempfile
import tomllib
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = 'xiaomi-band-11-4.100.139'
TARGETS = runpy.run_path(str(ROOT / 'scripts/verify-payload.py'))['TARGETS']


def firmware_test_commands(target, python):
    """Select only suites that actually support the exact requested AP."""
    if target not in TARGETS:
        raise ValueError(f'unsupported target: {target}')
    if target == 'xiaomi-band-10-pro-3.101.043':
        names = ('firmware_font_1043', 'firmware_quickapp_reload_1043')
    else:
        names = ('firmware_paths', 'firmware_restart', 'firmware_rebind',
                 'firmware_font_lifecycle', 'firmware_image_lifecycle',
                 'firmware_ui_redraw', 'firmware_page_rebuild',
                 'firmware_font_retarget', 'firmware_font_barrier_155',
                 'firmware_reload')
    commands = [(name, [python, str(ROOT / 'tests' / (name + '.py'))]) for name in names]
    # Calendar has an explicit exact-target CLI; QuickApp lookup tests all three
    # exact APs in one suite, so it is run once separately (not mislabeled .043).
    commands.append(('firmware_calendar',
                     [python, str(ROOT / 'tests/firmware_calendar.py'), '--target', target]))
    return commands


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='new delivery directory; existing paths are refused')
    parser.add_argument('--target', choices=TARGETS, action='append',
                        help='target to include; repeat for a multi-device bundle (default: .139)')
    args = parser.parse_args()
    targets = args.target or [TARGET]
    module_manifest = tomllib.loads((ROOT / 'Canopus.toml').read_text())
    project_id = module_manifest['module']['id']
    module_version = module_manifest['module']['version']
    if len(set(targets)) != len(targets):
        parser.error('duplicate target')
    output = args.output.absolute()
    if output.exists() or output.is_symlink():
        parser.error('output already exists; choose a new directory')
    default_canopus = ROOT.parent / 'Canopus'
    if not default_canopus.is_dir():
        default_canopus = ROOT.parent / 'Canopus-Private'
    canopus = Path(os.environ.get('CANOPUS_ROOT') or default_canopus).resolve()
    firmware_python = os.environ.get('FIRMWARE_PYTHON', str(canopus / 'build/band11-tests/bin/python'))
    if not shutil.which(firmware_python):
        parser.error('set FIRMWARE_PYTHON to a Python with the Canopus firmware-test dependencies')
    lua = os.environ.get('CANOPUS_TEST_LUA', 'lua')
    if not shutil.which(lua):
        parser.error('Lua is required; set CANOPUS_TEST_LUA')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=output.name + '.staging-', dir=output.parent) as temp:
        stage = Path(temp)
        env = dict(os.environ, CANOPUS_ROOT=str(canopus))
        steps = []
        log = stage / 'validation.log'

        def run(label, command, run_env=None):
            print(label, flush=True)
            result = subprocess.run(command, cwd=ROOT, env=run_env or env, capture_output=True, text=True)
            with log.open('a') as stream:
                stream.write('\n=== ' + label + ' ===\n' + result.stdout + result.stderr)
            if result.returncode:
                sys.stderr.write(result.stdout + result.stderr)
                raise RuntimeError(f'{label} failed (exit {result.returncode}); delivery not published')
            steps.append(label)

        for target in targets:
            payload = stage / 'payload' / target
            env['RESOURCE_HOOK_TARGET'] = target
            env['RESOURCE_HOOK_PAYLOAD'] = str(payload)
            run(f'{target}: Host sanitizers, ARM verifier and signed payload',
                ['sh', str(ROOT / 'scripts/build-install-payload.sh'), target, str(payload)])
            run(f'{target}: Delivery identity and tamper rejection',
                [sys.executable, str(ROOT / 'tests/test_delivery.py')])
            native_env = dict(env, RESOURCE_HOOK_TARGET=target)
            # Never inherit a different target's AP override in a multi bundle.
            if len(targets) > 1:
                native_env.pop('RESOURCE_HOOK_FIRMWARE', None)
            local_ap = ROOT / 'build/firmware-analysis' / f'vela_ap_{target.rsplit("-", 1)[1]}.bin'
            if not native_env.get('RESOURCE_HOOK_FIRMWARE') and local_ap.is_file():
                native_env['RESOURCE_HOOK_FIRMWARE'] = str(local_ap)
            for name, command in firmware_test_commands(target, firmware_python):
                run(f'{target}: {name}', command, native_env)
        run('All three exact targets: firmware_quickapp_icon',
            [firmware_python, str(ROOT / 'tests/firmware_quickapp_icon.py')])
        if any(target.startswith('xiaomi-band-11-') for target in targets):
            run('Band 11 .139/.155: firmware_font_compatibility',
                [firmware_python, str(ROOT / 'tests/firmware_font_compatibility.py')])
        run('Installer generation and restricted Lua protocol',
            [sys.executable, str(canopus / 'scripts/tests/test_module_installer_prod.py')])
        target_args = [arg for target in targets for arg in ('--target', target)]
        run('Watchface payload and Supervisor trust-key verification', [
            sys.executable, str(canopus / 'scripts/build_module_installer_prod.py'),
            '--product', 'resource-hook', *target_args, '--payload-dir', str(stage / 'payload'),
            '--assets-dir', str(ROOT / 'examples'), '--output-dir', str(stage / 'watchface')])
        for manifest_path in (stage / 'watchface').glob('*/build/manifest.json'):
            manifest = json.loads(manifest_path.read_text())
            manifest.update({
                'module_build_id': 'resource-hook-0.3.0', 'font_reload': True,
                'font_reload_device_status': 'USER_REPORTED_PASS',
                'hardware_status': 'NOT_PROBED', 'gpu_recovery': 'UNSUPPORTED',
                'framework_restart_safety': 'UNSUPPORTED',
            })
            manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
        source = stage / 'source'
        source.mkdir()
        for name in ('src', 'include', 'scripts', 'tests', 'tools', 'docs', 'examples', 'targets'):
            shutil.copytree(ROOT / name, source / name, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        for name in ('Canopus.toml', 'README.md', '.gitignore'):
            shutil.copyfile(ROOT / name, source / name)
        (stage / 'README.md').write_text(
            '# Resource Hook 0.3.0 integration delivery\n\n'
            f'Targets: {", ".join(targets)}. New artifact physical device: NOT_PROBED.\n\n'
            'Font reload: USER_REPORTED_PASS on all supported targets (user report). '
            'Transactional font reload is included by default; GPU recovery and framework '
            'restart safety remain unsupported.\n\n'
            '- Read INSTALL.md before installing or enabling.\n'
            '- payload/: signed ELF/CMI1, example mapping and verifier.\n'
            '- watchface/: installer Lua/resources; ZIP is input to a watchface packer, not a vendor watchface file.\n'
            '- source/: complete module sources; see source/README.md for build prerequisites.\n'
            '- evidence/: firmware lifecycle evidence and limitations.\n'
            '- validation.json and validation.log: checks run for this delivery.\n\n'
            'No automatic miwear restart, complete cache refresh or hardware-readiness claim.\n')
        shutil.copyfile(ROOT / 'docs/INSTALL.md', stage / 'INSTALL.md')
        shutil.copyfile(ROOT / 'docs/FONT_RELOAD.md', stage / 'FONT_RELOAD.md')
        evidence = stage / 'evidence'
        evidence.mkdir()
        for target in targets:
            destination = evidence if len(targets) == 1 else evidence / target
            destination.mkdir(exist_ok=True)
            for item in (ROOT / 'targets' / target).iterdir():
                if item.is_file() and item.suffix in ('.json', '.md'):
                    shutil.copyfile(item, destination / item.name)
        (stage / 'validation.json').write_text(json.dumps({
            'module': 'corona', 'version': module_version,
            'project_id': project_id,
            **({'target': targets[0]} if len(targets) == 1 else {}),
            'targets': targets, 'firmware_sha256': {target: TARGETS[target] for target in targets},
            'passed_steps': steps, 'physical_device': 'NOT_PROBED',
            'module_build_id': 'resource-hook-0.3.0', 'font_reload': True,
            'font_reload_device_status': 'USER_REPORTED_PASS',
            'gpu_recovery': 'UNSUPPORTED', 'framework_restart_safety': 'UNSUPPORTED',
            'native_firmware_suites': {
                target: [name for name, _ in firmware_test_commands(target, firmware_python)]
                for target in targets},
            'cross_target_suites': ['firmware_quickapp_icon'] + (
                ['firmware_font_compatibility']
                if any(target.startswith('xiaomi-band-11-') for target in targets) else []),
            'automatic_miwear_restart': False, 'complete_cache_refresh': False,
        }, indent=2) + '\n')
        files = sorted(p for p in stage.rglob('*') if p.is_file())
        (stage / 'SHA256SUMS').write_text(''.join(
            f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(stage).as_posix()}\n'
            for p in files))
        archive_target = targets[0] if len(targets) == 1 else (
            'xiaomi-band-11-4.100.139-4.100.155'
            if set(targets) == {'xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155'}
            else 'multi-device')
        archive = stage / f'resource-hook-0.3.0-{archive_target}.zip'
        with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as bundle:
            for path in files + [stage / 'SHA256SUMS']:
                bundle.write(path, path.relative_to(stage))
        (stage / (archive.name + '.sha256')).write_text(
            hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
        if output.exists() or output.is_symlink():
            raise RuntimeError('output appeared during build; refusing to overwrite')
        stage.rename(output)
    print(f'Signed release bundle (new artifacts NOT_PROBED): {output}')
    print(f'Archive: {output / archive.name}')
    print('Font reload: USER_REPORTED_PASS on supported targets; new artifact: NOT_PROBED.')
    print('GPU recovery/restart safety unsupported; no device was modified.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError) as error:
        sys.exit(str(error))
