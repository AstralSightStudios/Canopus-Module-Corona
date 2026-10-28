#!/usr/bin/env python3
"""Build a tested Resource Hook integration delivery (never deploys it)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import runpy
import re
import subprocess
import sys
import tempfile
import tomllib
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = 'xiaomi-band-11-4.100.155'
TARGETS = runpy.run_path(str(ROOT / 'scripts/verify-payload.py'))['TARGETS']
OPTIONS = runpy.run_path(str(ROOT / 'scripts/build_options.py'))


def firmware_test_commands(target, python):
    """Select only suites that actually support the exact requested AP."""
    if target not in TARGETS:
        raise ValueError(f'unsupported target: {target}')
    if target == 'redmi-watch-6-3.100.043':
        return [(name, [python, str(ROOT / 'tests' / (name + '.py'))])
                for name in ('firmware_p65', 'firmware_p65_features',
                             'firmware_p65_font', 'test_p65_calendar_native', 'test_p65_blur')]
    if target == 'xiaomi-band-11-4.100.179':
        # Selected native calendar instructions and the compiled adapter only.
        return [('test_calendar_native', [python, str(ROOT / 'tests/test_calendar_native.py')])]
    if target == 'xiaomi-band-11-4.100.177':
        # Existing native harnesses contain only .139/.155/.043 address maps.
        return []
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
                        help='target to include; repeat for a multi-target package (default: .155)')
    parser.add_argument('--firmware-tests', action='store_true',
                        help='also run exact-target firmware harnesses (requires firmware and dependencies)')
    args = parser.parse_args()
    targets = args.target or [TARGET]
    try:
        diagnostics = OPTIONS['build_options'](targets, os.environ)
    except ValueError as error:
        parser.error(str(error))
    module_manifest = tomllib.loads((ROOT / 'Canopus.toml').read_text())
    project_id = module_manifest['module']['id']
    module_version = module_manifest['module']['version']
    if len(set(targets)) != len(targets):
        parser.error('duplicate target')
    output = args.output.absolute()
    if output.exists() or output.is_symlink():
        parser.error('output already exists; choose a new directory')
    canopus = Path(os.environ.get('CANOPUS_ROOT') or ROOT.parent / 'Canopus-Private').resolve()
    firmware_targets = ([target for target in targets if firmware_test_commands(target, 'python')]
                        if args.firmware_tests else [])
    firmware_python = os.environ.get('FIRMWARE_PYTHON', str(canopus / 'build/band11-tests/bin/python'))
    if firmware_targets and not shutil.which(firmware_python):
        parser.error('set FIRMWARE_PYTHON to a Python with the Canopus firmware-test dependencies')
    cli = os.environ.get('CANOPUS_CLI')
    if not cli:
        cli = str(canopus / 'target/debug/canopus')
        if not Path(cli).is_file():
            cli = str(ROOT / 'build/canopus-cli/debug/canopus')
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
            env = OPTIONS['target_build_environment'](target,
                dict(os.environ, CANOPUS_ROOT=str(canopus)))
            env['RESOURCE_HOOK_TARGET'] = target
            env['RESOURCE_HOOK_PAYLOAD'] = str(payload)
            run(f'{target}: Host sanitizers, ARM verifier and CMI2 payload',
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
            if target == 'xiaomi-band-11-4.100.179':
                native_env['RH_CALENDAR_TARGET'] = '179'
            if args.firmware_tests:
                for name, command in firmware_test_commands(target, firmware_python):
                    run(f'{target}: {name}', command, native_env)
        legacy_firmware_targets = [target for target in firmware_targets
                                   if target in ('xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155',
                                                 'xiaomi-band-10-pro-3.101.043')]
        if legacy_firmware_targets:
            run('Native harness targets .139/.155/.043: firmware_quickapp_icon',
                [firmware_python, str(ROOT / 'tests/firmware_quickapp_icon.py')])
        if any(target in ('xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155') for target in firmware_targets):
            run('Band 11 .139/.155: firmware_font_compatibility',
                [firmware_python, str(ROOT / 'tests/firmware_font_compatibility.py')])
        # The canonical package is the installable deliverable. Module watchfaces
        # are a legacy transport; Supervisor bootstrap is managed separately.
        package_name = f'{project_id}-{module_version}.canopus'
        with tempfile.TemporaryDirectory(prefix='canopus-project-', dir=stage) as temporary:
            project = Path(temporary)
            selected, count = re.subn(r'(?ms)^\[targets\][^\n]*\n.*?(?=^\[|\Z)',
                '[targets]\ninclude = ' + json.dumps(targets) + '\n',
                (ROOT / 'Canopus.toml').read_text())
            if count != 1:
                raise RuntimeError('expected one [targets] table in Canopus.toml')
            manifest_path = project / 'Canopus.toml'
            manifest_path.write_text(selected)
            OPTIONS['stage_resources'](ROOT, project, module_manifest, targets)
            for target in targets:
                artifact = project / 'build' / target / f'{project_id}.elf'
                artifact.parent.mkdir(parents=True)
                shutil.copyfile(stage / 'payload' / target / 'resource-hook.elf', artifact)
            run('Native Canopus package', [cli, 'package', '--manifest', str(manifest_path),
                                          '--output', str(stage / package_name)])
            run('Native package structure and hashes',
                [cli, 'package', 'verify', str(stage / package_name)])
        source = stage / 'source'
        source.mkdir()
        for name in ('src', 'include', 'scripts', 'tests', 'tools', 'docs', 'examples', 'targets'):
            shutil.copytree(ROOT / name, source / name, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        # The native Manager asset generator needs these original PNG inputs.
        shutil.copytree(ROOT / 'assets/manager', source / 'assets/manager')
        for name in ('Canopus.toml', 'README.md', '.gitignore'):
            shutil.copyfile(ROOT / name, source / name)
        (stage / 'README.md').write_text(
            '# Resource Hook 0.3.0 integration delivery\n\n'
            f'Targets: {", ".join(targets)}. New artifact physical device: NOT_PROBED.\n\n'
            'Font reload: legacy targets have user reports; .177/.179 are NOT_PROBED. '
            'P65 font reload, native backgrounds, blur and calendar are included by default; device NOT_PROBED. '
            'Supported targets include transactional font reload by default; GPU recovery and framework '
            'restart safety remain unsupported. See validation.json target_features for exact support.\n\n'
            '- Read INSTALL.md before installing or enabling.\n'
            f'- {package_name}: install through AstroBox Canopus using native L2/CPC2.\n'
            '- payload/: per-target ELF/CMI2, Canopus package, example mapping and verifier.\n'
            '- Supervisor bootstrap is separate from module installation.\n'
            '- The package includes the native Corona Manager and its built-in images; theme packs are separate.\n'
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
            **OPTIONS['background_metadata'](targets, diagnostics),
            **OPTIONS['feature_metadata'](targets),
            'module': 'corona', 'version': module_version,
            'project_id': project_id,
            **({'target': targets[0]} if len(targets) == 1 else {}),
            'targets': targets, 'firmware_sha256': {target: TARGETS[target] for target in targets},
            'passed_steps': steps, 'physical_device': 'NOT_PROBED',
            'module_build_id': 'resource-hook-0.3.0',
            'receipt_format_version': 2,
            'delivery_format': 'canopus', 'package': package_name,
            'transport': 'wearpacket-l2-cpc2',
            'gpu_recovery': 'UNSUPPORTED', 'framework_restart_safety': 'UNSUPPORTED',
            'native_firmware_suites': {
                target: [name for name, _ in firmware_test_commands(target, firmware_python)]
                for target in firmware_targets},
            'native_firmware_suites_not_run': {
                target: ('No exact-target firmware harness' if not firmware_test_commands(target, firmware_python)
                         else 'Not requested; use --firmware-tests')
                for target in targets if target not in firmware_targets},
            'cross_target_suites': (['firmware_quickapp_icon'] if legacy_firmware_targets else []) + (
                ['firmware_font_compatibility']
                if any(target in ('xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155')
                       for target in firmware_targets) else []),
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
    print(f'Release bundle (new artifacts NOT_PROBED): {output}')
    print(f'Installable module: {output / package_name}')
    print(f'Archive: {output / archive.name}')
    print('Font reload: enabled by default; P65/.177/.179 and new artifacts NOT_PROBED.')
    print('GPU recovery/restart safety unsupported; no device was modified.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError) as error:
        sys.exit(str(error))
