#!/usr/bin/env python3
"""Legacy module watchface builder; use build-delivery.py for native .canopus distribution."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CANOPUS = ROOT.parent / 'Canopus-Private'
CANOPUS = Path(os.environ.get('CANOPUS_ROOT', DEFAULT_CANOPUS)).resolve()
DEFAULT_TARGETS = ['xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155']
P67_TARGET = 'xiaomi-band-10-pro-3.101.043'
TARGETS = DEFAULT_TARGETS + [P67_TARGET, 'xiaomi-band-11-4.100.177']
WATCHFACE_BUILDER = CANOPUS / 'scripts/build_module_installer_prod.py'


def run(command, *, env=None):
    subprocess.run([str(arg) for arg in command], cwd=ROOT, env=env, check=True)


def copy_tree(source, destination):
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '.DS_Store'))


def device_groups(targets):
    groups = {}
    for target in targets:
        if target not in TARGETS:
            raise ValueError(f'unsupported target: {target}')
        groups.setdefault(target.rsplit('-', 1)[0], []).append(target)
    return groups


OPTIONS = runpy.run_path(str(ROOT / 'scripts/build_options.py'))
build_options = OPTIONS['build_options']
target_build_environment = OPTIONS['target_build_environment']
background_metadata = OPTIONS['background_metadata']


def default_output_name(targets):
    prefix = 'module-installer-resource-hook-0.3.0-'
    if set(targets) == set(DEFAULT_TARGETS):
        return prefix + 'band11'
    if len(targets) == 1:
        family = 'band10pro' if targets[0].startswith('xiaomi-band-10-pro-') else 'band11'
        return prefix + family + '-' + targets[0].rsplit('-', 1)[1]
    return prefix + 'multi-device'


def verify_watchface(device, targets, modules, receipts, *, archive_path=None):
    expected = {'main.lua'}
    for target in targets:
        for suffix, payloads in (('.bin', modules), ('.cmi.bin', receipts)):
            name = f'resource-hook-{target}{suffix}'
            expected.add(name)
            if (device / name).read_bytes() != payloads[target].read_bytes():
                raise ValueError(f'watchface payload differs from verified payload for {target}: {name}')
    # Each device folder must be independently packable, with no foreign payloads.
    if {p.name for p in device.iterdir() if p.is_file()} != expected:
        raise ValueError(f'unexpected watchface resource contents: {device.name}')
    manifest = json.loads((device / 'build/manifest.json').read_text())
    if {item['id'] for item in manifest['targets'].values()} != set(targets):
        raise ValueError(f'watchface manifest targets differ: {device.name}')
    if archive_path is not None:
        with zipfile.ZipFile(archive_path) as archive:
            if set(archive.namelist()) != expected or len(archive.namelist()) != len(expected):
                raise ValueError(f'unexpected watchface archive contents: {device.name}')
            for name in expected:
                if archive.read(name) != (device / name).read_bytes():
                    raise ValueError(f'watchface ZIP resource differs from verified source: {name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=None,
                        help='new output directory (default name follows selected device/firmware)')
    parser.add_argument('--target', choices=TARGETS, action='append',
                        help='target to include; repeat for multi-target bundle (default: both Band 11 targets)')
    parser.add_argument('--overwrite', action='store_true',
                        help='overwrite existing output directory if it already exists')
    args = parser.parse_args()
    targets = args.target or list(DEFAULT_TARGETS)
    if len(set(targets)) != len(targets):
        parser.error('duplicate target')
    try:
        diagnostics = build_options(targets, os.environ)
    except ValueError as error:
        parser.error(str(error))
    module_description = 'normal module with transactional font reload and background adapters'
    p67_note = ('\n\nP67: screen-fixed backgrounds for launcher, notifications, Control Center '
                'and widgets are included by default. Native foreground remains live; '
                'transition snapshots remain firmware-owned. Blur uses module-owned '
                'BGR888 input and RGB565 output, with no native pool access or inline patch. '
                'This new artifact and GPU/cache/frame-time behavior are NOT_PROBED. '
                'Restore system while idle; do not unload while callbacks are attached.\n')
    background_note = p67_note if P67_TARGET in targets else ''
    groups = device_groups(targets)

    if args.output is None:
        output_name = default_output_name(targets)
        output = (ROOT / 'dist' / output_name).expanduser().absolute()
    else:
        output = args.output.expanduser().absolute()

    if (output.exists() or output.is_symlink()) and not args.overwrite:
        parser.error(f'output already exists; refusing to overwrite: {output}')
    if not WATCHFACE_BUILDER.is_file():
        parser.error(f'Canopus production installer tooling not found under: {CANOPUS}')
    output.parent.mkdir(parents=True, exist_ok=True)

    import tomllib
    firmwares = {}
    for target in targets:
        firmware_sha = CANOPUS / 'targets' / target / 'target.toml'
        firmwares[target] = tomllib.loads(firmware_sha.read_text())['firmware_sha256']

    with tempfile.TemporaryDirectory(prefix=output.name + '.staging-', dir=output.parent) as temporary:
        stage = Path(temporary)

        modules = {}
        receipts = {}
        for target in targets:
            env = target_build_environment(target, os.environ)
            env['CANOPUS_ROOT'] = str(CANOPUS)
            env['RESOURCE_HOOK_TARGET'] = target
            payload = stage / 'payload' / target
            # Always rebuild and package through the common payload entrypoint.
            run(['sh', ROOT / 'scripts/build-install-payload.sh', target, payload], env=env)
            module = payload / 'resource-hook.elf'
            receipt = payload / 'receipt.bin'
            modules[target] = module
            receipts[target] = receipt

        generated = stage / 'watchface'
        target_args = [arg for target in targets for arg in ('--target', target)]
        run([sys.executable, WATCHFACE_BUILDER, '--product', 'resource-hook',
             '--module-id', 'corona',
             *target_args, '--payload-dir', stage / 'payload',
             '--assets-dir', ROOT / 'examples', '--output-dir', generated])
        for device_name, device_targets in groups.items():
            device = generated / device_name
            verify_watchface(device, device_targets, modules, receipts)
            font_status = ('NOT_PROBED' if 'xiaomi-band-11-4.100.177' in device_targets
                           else 'USER_REPORTED_PASS')
            target_versions = ', '.join(t.rsplit('-', 1)[1] for t in device_targets)
            device_docs = device / 'docs/README.md'
            device_docs.write_text(
                f'# Resource Hook installer\n\n'
                f'Targets: {device_name}, firmware {target_versions}. This watchface installs '
                f'the {module_description} (`resource-hook-0.3.0`) in the disabled state.\n\n'
                'Pack only `main.lua` and the .bin files in this directory. '
                'Requires the current Canopus Supervisor with CMI2 support. '
                'Opening the watchface installs the module; upgrades take effect after reboot.\n\n'
                'Use Manager 1.2.2 or newer for transaction acknowledgements; install it separately. '
                f'Font reload: {font_status}. This new artifact: '
                'NOT_PROBED on hardware. GPU recovery and framework restart safety remain '
                'unsupported. Never force-release native font caches.\n' +
                (p67_note if P67_TARGET in device_targets else ''))
            manifest_path = device / 'build/manifest.json'
            manifest = json.loads(manifest_path.read_text())
            manifest['font_reload'] = True
            manifest['font_reload_device_status'] = font_status
            manifest['gpu_recovery'] = 'UNSUPPORTED'
            manifest['framework_restart_safety'] = 'UNSUPPORTED'
            manifest['module_build_id'] = 'resource-hook-0.3.0'
            manifest['receipt_format_version'] = 2
            manifest['hardware_status'] = 'NOT_PROBED'
            manifest.update(background_metadata(device_targets, diagnostics))
            manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
            # Recreate and verify each independently packable device archive.
            archive_path = device / 'build/resource-hook-prod.zip'
            with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED) as archive:
                archive.write(device / 'main.lua', 'main.lua')
                for target in device_targets:
                    for suffix in ('.bin', '.cmi.bin'):
                        name = f'resource-hook-{target}{suffix}'
                        archive.write(device / name, name)
            verify_watchface(device, device_targets, modules, receipts, archive_path=archive_path)

        package = stage / 'package'
        package.mkdir()
        (package / 'docs').mkdir()
        (package / 'docs/README.md').write_text(
            f'# Resource Hook installer watchface\n\n'
            f'This complete package targets {", ".join(targets)} and installs '
            f'the {module_description}.\n\n'
            'Open the appropriate device folder in the watchface packer, or use its '
            '`build/resource-hook-prod.zip` as the flat watchface input. Keep the '
            '`.cmi.bin` beside the module `.bin`.\n\n'
            'Receipts are unsigned CMI2 generated by `canopus package`. '
            'Install the current Canopus Supervisor before using this package.\n\n'
            'Manager 1.2.2 or newer is recommended for revision-matched transaction '
            'results. Font reload: legacy targets have user reports; .177 is NOT_PROBED. '
            'This new package is NOT_PROBED on hardware. GPU recovery and framework '
            'restart safety remain unsupported.\n' + background_note)
        shutil.copyfile(ROOT / 'docs/FONT_RELOAD.md', package / 'docs/FONT_RELOAD.md')
        for target in targets:
            evidence = package / 'evidence' / target
            evidence.mkdir(parents=True)
            for item in (ROOT / 'targets' / target).iterdir():
                if item.is_file() and item.suffix in ('.json', '.md'):
                    shutil.copyfile(item, evidence / item.name)
        for device_name in groups:
            copy_tree(generated / device_name, package / device_name)
        (package / 'README.md').write_text(
            f'# Resource Hook installer ({", ".join(groups)})\n\n'
            'Build output from the latest checked `resource-hook-0.3.0` ELF. '
            'The unsigned CMI2 receipt binds the artifact to its exact firmware target.\n\n'
            + ''.join(f'- `{name}/`: complete watchface source/resources and ready-to-pack ZIP.\n'
                      f'- `{name}/build/manifest.json`: target and resource hashes.\n'
                      for name in groups) +
            '- `SHA256SUMS`: package file checksums.\n\n'
            'Font reload: legacy targets have user reports; .177 is NOT_PROBED. '
            'This new artifact is NOT_PROBED. GPU recovery and framework restart safety '
            'remain unsupported. Do not use on other firmware targets.\n' + background_note)
        target_details = {}
        for target in targets:
            target_details[target] = {
                'firmware_sha256': firmwares[target],
                'module_sha256': hashlib.sha256(modules[target].read_bytes()).hexdigest(),
                'receipt_sha256': hashlib.sha256(receipts[target].read_bytes()).hexdigest(),
            }
        build_meta = {
            'name': output.name, 'version': '0.3.0',
            'runtime_id': 'corona', 'project_id': 'corona',
            'targets': targets,
            'module_build_id': 'resource-hook-0.3.0',
            'target_details': target_details,
            'receipt_format_version': 2,
            'hardware_status': 'NOT_PROBED',
            'font_reload': True, 'font_reload_device_status': (
                'NOT_PROBED' if 'xiaomi-band-11-4.100.177' in targets else 'USER_REPORTED_PASS'),
            'gpu_recovery': 'UNSUPPORTED', 'framework_restart_safety': 'UNSUPPORTED',
        }
        build_meta.update(background_metadata(targets, diagnostics))
        if P67_TARGET in targets:
            shutil.copyfile(ROOT / 'docs/P67_BACKGROUNDS.md', package / 'docs/P67_BACKGROUNDS.md')
        if len(targets) == 1:
            build_meta['target'] = targets[0]
            build_meta['firmware_sha256'] = firmwares[targets[0]]
            build_meta['module_sha256'] = target_details[targets[0]]['module_sha256']
            build_meta['receipt_sha256'] = target_details[targets[0]]['receipt_sha256']
        (package / 'build.json').write_text(json.dumps(build_meta, indent=2) + '\n')
        files = sorted(item for item in package.rglob('*') if item.is_file())
        (package / 'SHA256SUMS').write_text(''.join(
            f'{hashlib.sha256(item.read_bytes()).hexdigest()}  {item.relative_to(package).as_posix()}\n'
            for item in files))
        bundle = package / f'{output.name}.zip'
        with zipfile.ZipFile(bundle, 'w', zipfile.ZIP_DEFLATED) as archive:
            for item in sorted(p for p in package.rglob('*') if p.is_file() and p != bundle):
                archive.write(item, item.relative_to(package))

        # Check the final copied watchfaces, not merely the builder staging output.
        for device_name, device_targets in groups.items():
            device = package / device_name
            verify_watchface(device, device_targets, modules, receipts,
                             archive_path=device / 'build/resource-hook-prod.zip')

        if output.exists() or output.is_symlink():
            if not args.overwrite:
                raise RuntimeError(f'output appeared during build; refusing to replace: {output}')
            shutil.rmtree(output)
        package.rename(output)

    print(f'Complete watchface package: {output}')
    for device_name in groups:
        print(f'Watchface input ZIP: {output}/{device_name}/build/resource-hook-prod.zip')
    for target in targets:
        elf_sha = hashlib.sha256((output / target.rsplit('-', 1)[0] / f'resource-hook-{target}.bin').read_bytes()).hexdigest()
        print(f'ELF SHA-256 ({target}): {elf_sha}')
    print('Unsigned CMI2 receipts. Font reload: legacy user reports; .177 NOT_PROBED.')
    print('New artifact: NOT_PROBED. GPU recovery/restart safety unsupported.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        sys.exit(str(error))
