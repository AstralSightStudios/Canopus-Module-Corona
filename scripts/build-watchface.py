#!/usr/bin/env python3
"""Build signed Resource Hook watchfaces for exact supported firmware targets."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CANOPUS = ROOT.parent / 'Canopus'
if not DEFAULT_CANOPUS.is_dir():
    DEFAULT_CANOPUS = ROOT.parent / 'Canopus-Private'
CANOPUS = Path(os.environ.get('CANOPUS_ROOT', DEFAULT_CANOPUS)).resolve()
DEFAULT_TARGETS = ['xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155']
TARGETS = DEFAULT_TARGETS + ['xiaomi-band-10-pro-3.101.043']
DEFAULT_CERTS = [
    Path.home() / 'develop/Astrobox-certs/module-installer-ed25519.pem.zip',
    Path.home() / 'develop/AstroBox-Certs/module-installer-ed25519.pem.zip',
]
DEFAULT_CERT = next((p for p in DEFAULT_CERTS if p.is_file()), DEFAULT_CERTS[0])
SUPERVISOR = CANOPUS / 'manager/service/canopus_supervisor_platform.c'
RECEIPT_BUILDER = CANOPUS / 'scripts/build-module-installer-receipt.py'
WATCHFACE_BUILDER = CANOPUS / 'scripts/build_module_installer_prod.py'


def run(command, *, env=None):
    subprocess.run([str(arg) for arg in command], cwd=ROOT, env=env, check=True)


def trusted_public_key():
    source = SUPERVISOR.read_text()
    match = re.search(r's_installer_public_key\[32\]\s*=\s*\{(.*?)\};', source, re.S)
    if not match:
        raise ValueError('Cannot locate the resident Supervisor installer trust key')
    key = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})', match[1]))
    if len(key) != 32:
        raise ValueError('Invalid resident Supervisor Ed25519 trust key')
    return key


def extract_private_key(archive_path, key_path):
    with zipfile.ZipFile(archive_path) as archive:
        files = [item for item in archive.infolist() if not item.is_dir()]
        if len(files) != 1 or files[0].filename != 'module-installer-ed25519.pem':
            raise ValueError('Expected only module-installer-ed25519.pem in the AstroBox certificate ZIP')
        item = files[0]
        if item.file_size > 16384 or item.flag_bits & 1:
            raise ValueError('Unexpected or encrypted signing-key archive entry')
        mode = item.external_attr >> 16
        if stat.S_ISLNK(mode):
            raise ValueError('Signing-key ZIP entry must not be a symlink')
        key_path.write_bytes(archive.read(item))
    key_path.chmod(0o600)


def assert_key_matches_supervisor(private_key, expected, work):
    public_der = work / 'signer-public.der'
    public_pem = work / 'signer-public.pem'
    subprocess.run(['openssl', 'pkey', '-in', str(private_key), '-pubout',
                    '-outform', 'DER', '-out', str(public_der)],
                   check=True, capture_output=True)
    raw = public_der.read_bytes()
    if len(raw) < 32 or raw[-32:] != expected:
        raise ValueError('Selected AstroBox signing key does not match the installed Supervisor')
    subprocess.run(['openssl', 'pkey', '-in', str(private_key), '-pubout',
                    '-out', str(public_pem)], check=True, capture_output=True)
    return public_pem, hashlib.sha256(expected).hexdigest()


def copy_tree(source, destination):
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '.DS_Store'))


def device_groups(targets):
    groups = {}
    for target in targets:
        if target not in TARGETS:
            raise ValueError(f'unsupported target: {target}')
        groups.setdefault(target.rsplit('-', 1)[0], []).append(target)
    return groups


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
    parser.add_argument('--certificate-zip', type=Path, default=DEFAULT_CERT,
                        help='AstroBox module-installer Ed25519 certificate ZIP')
    parser.add_argument('--overwrite', action='store_true',
                        help='overwrite existing output directory if it already exists')
    args = parser.parse_args()
    targets = args.target or list(DEFAULT_TARGETS)
    groups = device_groups(targets)
    if len(set(targets)) != len(targets):
        parser.error('duplicate target')

    if args.output is None:
        output_name = default_output_name(targets)
        output = (ROOT / 'dist' / output_name).expanduser().absolute()
    else:
        output = args.output.expanduser().absolute()

    cert = args.certificate_zip.expanduser().resolve()
    if (output.exists() or output.is_symlink()) and not args.overwrite:
        parser.error(f'output already exists; refusing to overwrite: {output}')
    if not cert.is_file():
        parser.error(f'certificate ZIP not found: {cert}')
    if not (CANOPUS.is_dir() and SUPERVISOR.is_file() and RECEIPT_BUILDER.is_file()
            and WATCHFACE_BUILDER.is_file()):
        parser.error(f'Canopus production installer tooling not found under: {CANOPUS}')
    output.parent.mkdir(parents=True, exist_ok=True)

    import tomllib
    firmwares = {}
    for target in targets:
        firmware_sha = CANOPUS / 'targets' / target / 'target.toml'
        firmwares[target] = tomllib.loads(firmware_sha.read_text())['firmware_sha256']
    private_trust = trusted_public_key()

    with tempfile.TemporaryDirectory(prefix=output.name + '.staging-', dir=output.parent) as temporary:
        stage = Path(temporary)
        secure = stage / '.signing'
        secure.mkdir(mode=0o700)
        private_key = secure / 'module-installer-ed25519.pem'
        extract_private_key(cert, private_key)
        public_key, public_fingerprint = assert_key_matches_supervisor(
            private_key, private_trust, secure)

        modules = {}
        receipts = {}
        for target in targets:
            env = dict(os.environ)
            env['CANOPUS_ROOT'] = str(CANOPUS)
            env['RESOURCE_HOOK_TARGET'] = target
            # Always rebuild: target/build ID alone cannot distinguish an old
            # registry-only artifact from the current font-enabled module.
            run(['sh', ROOT / 'scripts/build.sh', target], env=env)
            elf = ROOT / 'build/resource-hook.elf'
            if not elf.is_file():
                raise ValueError(f'missing normal module ELF for {target}: {elf}')

            payload = stage / 'payload' / target
            payload.mkdir(parents=True)
            module = payload / 'resource-hook.elf'
            shutil.copyfile(elf, module)
            # Validate the exact staged bytes before signing, not just the
            # mutable build-directory artifact or a self-consistent receipt.
            run([env.get('CANOPUS_CLI', str(CANOPUS / 'target/debug/canopus')),
                 'verify', module, '--target', target,
                 '--targets-dir', CANOPUS / 'targets'], env=env)
            receipt = payload / 'receipt.bin'
            run([sys.executable, RECEIPT_BUILDER,
                 '--module', module, '--module-id', 'corona', '--version', 3,
                 '--lifecycle', 1, '--target-id', target, '--firmware-sha256', firmwares[target],
                 '--private-key', private_key, '--output', receipt])
            run([sys.executable, ROOT / 'scripts/verify-payload.py', payload,
                 '--target', target, '--public-key', public_key])
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
            target_versions = ', '.join(t.rsplit('-', 1)[1] for t in device_targets)
            device_docs = device / 'docs/README.md'
            device_docs.write_text(
                f'# Resource Hook installer\n\n'
                f'Targets: {device_name}, firmware {target_versions}. This watchface installs '
                'the normal `resource-hook-0.3.0` module in the disabled state.\n\n'
                'Pack only `main.lua` and the .bin files in this directory. '
                'Requires a Canopus Supervisor whose Ed25519 trust key matches this receipt. '
                'Opening the watchface installs the module; it does not enable/restart the framework.\n\n'
                'Use Manager 1.2.2 or newer for transaction acknowledgements; install it separately. '
                'Font reload: USER_REPORTED_PASS on all supported targets. This new artifact: '
                'NOT_PROBED on hardware. GPU recovery and framework restart safety remain '
                'unsupported. Never force-release native font caches.\n')
            manifest_path = device / 'build/manifest.json'
            manifest = json.loads(manifest_path.read_text())
            manifest['font_reload'] = True
            manifest['font_reload_device_status'] = 'USER_REPORTED_PASS'
            manifest['gpu_recovery'] = 'UNSUPPORTED'
            manifest['framework_restart_safety'] = 'UNSUPPORTED'
            manifest['module_build_id'] = 'resource-hook-0.3.0'
            manifest['signer_public_key_sha256'] = public_fingerprint
            manifest['hardware_status'] = 'NOT_PROBED'
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
            f'# Signed Resource Hook installer watchface\n\n'
            f'This complete package targets {", ".join(targets)} and installs '
            f'the normal module with transactional font reload.\n\n'
            'Open the appropriate device folder in the watchface packer, or use its '
            '`build/resource-hook-prod.zip` as the flat watchface input. Keep the '
            '`.cmi.bin` beside the module `.bin`.\n\n'
            f'Signer: AstroBox module-installer Ed25519 certificate '
            f'(public SHA-256 `{public_fingerprint}`). The public key matches the '
            'Supervisor trust key. Private signing material is not included.\n\n'
            'Manager 1.2.2 or newer is recommended for revision-matched transaction '
            'results. Font reload: USER_REPORTED_PASS on all supported targets (user report). '
            'This new package is NOT_PROBED on hardware. GPU recovery and framework '
            'restart safety remain unsupported.\n')
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
            'The module receipt is signed by the user-selected AstroBox certificate and '
            'verified against the Canopus Supervisor public trust key.\n\n'
            + ''.join(f'- `{name}/`: complete watchface source/resources and ready-to-pack ZIP.\n'
                      f'- `{name}/build/manifest.json`: target, resource hashes and signer identity.\n'
                      for name in groups) +
            '- `SHA256SUMS`: package file checksums.\n\n'
            'Font reload: USER_REPORTED_PASS on all supported targets (user report). '
            'This new artifact is NOT_PROBED. GPU recovery and framework restart safety '
            'remain unsupported. Do not use on other firmware targets.\n')
        target_details = {}
        for target in targets:
            target_details[target] = {
                'firmware_sha256': firmwares[target],
                'module_sha256': hashlib.sha256(modules[target].read_bytes()).hexdigest(),
                'receipt_sha256': hashlib.sha256(receipts[target].read_bytes()).hexdigest(),
            }
        build_meta = {
            'name': output.name, 'version': '0.3.0',
            'runtime_id': 'corona', 'project_id': 'ng.lst.corona',
            'targets': targets,
            'module_build_id': 'resource-hook-0.3.0',
            'target_details': target_details,
            'signer_public_key_sha256': public_fingerprint,
            'signer_certificate_archive': cert.name,
            'hardware_status': 'NOT_PROBED',
            'font_reload': True, 'font_reload_device_status': 'USER_REPORTED_PASS',
            'gpu_recovery': 'UNSUPPORTED', 'framework_restart_safety': 'UNSUPPORTED',
        }
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

    print(f'Complete signed watchface package: {output}')
    for device_name in groups:
        print(f'Watchface input ZIP: {output}/{device_name}/build/resource-hook-prod.zip')
    for target in targets:
        elf_sha = hashlib.sha256((output / target.rsplit('-', 1)[0] / f'resource-hook-{target}.bin').read_bytes()).hexdigest()
        print(f'ELF SHA-256 ({target}): {elf_sha}')
    print(f'Supervisor-matched AstroBox signing key SHA-256: {public_fingerprint}')
    print('Private key excluded. Font reload: USER_REPORTED_PASS on supported targets.')
    print('New artifact: NOT_PROBED. GPU recovery/restart safety unsupported.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        sys.exit(str(error))
