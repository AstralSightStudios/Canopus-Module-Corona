#!/usr/bin/env python3
"""Build a complete signed Band 11 .155 experimental-font watchface bundle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CANOPUS = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus-Private')).resolve()
TARGET = 'xiaomi-band-11-4.100.155'
DEFAULT_CERT = Path.home() / 'develop/AstroBox-Certs/module-installer-ed25519.pem.zip'
SUPERVISOR = CANOPUS / 'manager/service/canopus_supervisor_platform.c'
RECEIPT_BUILDER = CANOPUS / 'scripts/build-module-installer-receipt.py'
WATCHFACE_BUILDER = CANOPUS / 'scripts/build_module_installer_prod.py'
OUTPUT_NAME = 'module-installer-font-experimental-0.3.0-band11-4.100.155-signed'


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'dist' / OUTPUT_NAME,
                        help='new output directory (must not already exist)')
    parser.add_argument('--certificate-zip', type=Path, default=DEFAULT_CERT,
                        help='AstroBox module-installer Ed25519 certificate ZIP')
    parser.add_argument('--no-build', action='store_true',
                        help='package the already-built opt-in ELF; strict verification still runs')
    args = parser.parse_args()
    output = args.output.expanduser().absolute()
    cert = args.certificate_zip.expanduser().resolve()
    if output.exists() or output.is_symlink():
        parser.error(f'output already exists; refusing to overwrite: {output}')
    if not cert.is_file():
        parser.error(f'certificate ZIP not found: {cert}')
    if not (CANOPUS.is_dir() and SUPERVISOR.is_file() and RECEIPT_BUILDER.is_file()
            and WATCHFACE_BUILDER.is_file()):
        parser.error(f'Canopus production installer tooling not found under: {CANOPUS}')
    output.parent.mkdir(parents=True, exist_ok=True)
    firmware_sha = CANOPUS / 'targets' / TARGET / 'target.toml'
    import tomllib
    firmware = tomllib.loads(firmware_sha.read_text())['firmware_sha256']
    private_trust = trusted_public_key()

    with tempfile.TemporaryDirectory(prefix=output.name + '.staging-', dir=output.parent) as temporary:
        stage = Path(temporary)
        secure = stage / '.signing'
        secure.mkdir(mode=0o700)
        private_key = secure / 'module-installer-ed25519.pem'
        extract_private_key(cert, private_key)
        public_key, public_fingerprint = assert_key_matches_supervisor(
            private_key, private_trust, secure)

        env = dict(os.environ)
        env['CANOPUS_ROOT'] = str(CANOPUS)
        env['RESOURCE_HOOK_TARGET'] = TARGET
        env['RH_EXPERIMENTAL_FONT_RELOAD'] = '1'
        if not args.no_build:
            run(['sh', ROOT / 'scripts/build.sh', TARGET], env=env)
        elf = ROOT / 'build/resource-hook-font-experimental.elf'
        if not elf.is_file():
            raise ValueError(f'missing opt-in module ELF: {elf}')

        payload = stage / 'payload' / TARGET
        payload.mkdir(parents=True)
        module = payload / 'resource-hook.elf'
        shutil.copyfile(elf, module)
        receipt = payload / 'receipt.bin'
        run([sys.executable, RECEIPT_BUILDER,
             '--module', module, '--module-id', 'resource_hook', '--version', 3,
             '--lifecycle', 1, '--target-id', TARGET, '--firmware-sha256', firmware,
             '--private-key', private_key, '--output', receipt])
        run([sys.executable, ROOT / 'scripts/verify-payload.py', payload,
             '--target', TARGET, '--public-key', public_key])

        generated = stage / 'watchface'
        run([sys.executable, WATCHFACE_BUILDER, '--product', 'resource-hook',
             '--target', TARGET, '--payload-dir', stage / 'payload',
             '--assets-dir', ROOT / 'examples', '--output-dir', generated])
        device = generated / 'xiaomi-band-11'
        bundled_module = device / f'resource-hook-{TARGET}.bin'
        bundled_receipt = device / f'resource-hook-{TARGET}.cmi.bin'
        if bundled_module.read_bytes() != module.read_bytes():
            raise ValueError('watchface does not contain the exact verified experimental ELF')
        if bundled_receipt.read_bytes() != receipt.read_bytes():
            raise ValueError('watchface receipt differs from the AstroBox-signed receipt')

        device_docs = device / 'docs/README.md'
        device_docs.write_text(
            '# Resource Hook experimental-font installer\n\n'
            'Target: Xiaomi Band 11, firmware 4.100.155 only. This watchface installs '
            'the opt-in `resource-hook-0.3.0-font-exp` module in the disabled state.\n\n'
            'Pack only `main.lua` and the three `.bin` files in this directory. '
            'Requires a Canopus Supervisor whose Ed25519 trust key matches this receipt. '
            'Opening the watchface installs the module; it does not enable/restart the framework.\n\n'
            'Font reload remains experimental: use Manager 1.2.2 or newer for transaction '
            'acknowledgements. Install the Manager separately. On-device display/GPU and '
            'restart recovery have not been verified. Never force-release native font caches.\n')
        manifest_path = device / 'build/manifest.json'
        manifest = json.loads(manifest_path.read_text())
        manifest['experimental_font_reload'] = True
        manifest['module_build_id'] = 'resource-hook-0.3.0-font-exp'
        manifest['signer_public_key_sha256'] = public_fingerprint
        manifest['hardware_status'] = 'NOT_PROBED'
        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
        # Refresh the watchface archive after adding experimental metadata.
        archive_path = device / 'build/resource-hook-prod.zip'
        with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name in ('main.lua', f'resource-hook-{TARGET}.bin',
                         f'resource-hook-{TARGET}.cmi.bin'):
                archive.write(device / name, name)

        package = stage / 'package'
        package.mkdir()
        (package / 'docs').mkdir()
        (package / 'docs/README.md').write_text(
            '# Signed experimental-font installer watchface\n\n'
            'This complete package targets Xiaomi Band 11 firmware 4.100.155 and installs '
            'the opt-in `.155` experimental font-reload module.\n\n'
            'Open `xiaomi-band-11/` in the watchface packer, or use its '
            '`build/resource-hook-prod.zip` as the flat watchface input. Keep the '
            '`.cmi.bin` beside the module `.bin`.\n\n'
            f'Signer: AstroBox module-installer Ed25519 certificate '
            f'(public SHA-256 `{public_fingerprint}`). The public key matches the '
            'Supervisor trust key. Private signing material is not included.\n\n'
            'Manager 1.2.2 or newer is recommended for revision-matched transaction '
            'results. This package is NOT_PROBED on hardware. GPU recovery and framework '
            'restart safety remain unsupported.\n')
        copy_tree(device, package / 'xiaomi-band-11')
        (package / 'README.md').write_text(
            '# Resource Hook .155 experimental font installer\n\n'
            'Build output from the latest checked `resource-hook-0.3.0-font-exp` ELF. '
            'The module receipt is signed by the user-selected AstroBox certificate and '
            'verified against the Canopus Supervisor public trust key.\n\n'
            '- `xiaomi-band-11/`: complete watchface source/resources and ready-to-pack ZIP.\n'
            '- `xiaomi-band-11/build/manifest.json`: target, resource hashes and signer identity.\n'
            '- `SHA256SUMS`: package file checksums.\n\n'
            'Hardware acceptance is NOT_PROBED. Do not use on other firmware targets.\n')
        (package / 'build.json').write_text(json.dumps({
            'name': OUTPUT_NAME, 'target': TARGET, 'firmware_sha256': firmware,
            'module_build_id': 'resource-hook-0.3.0-font-exp',
            'module_sha256': hashlib.sha256(module.read_bytes()).hexdigest(),
            'receipt_sha256': hashlib.sha256(receipt.read_bytes()).hexdigest(),
            'signer_public_key_sha256': public_fingerprint,
            'signer_certificate_archive': cert.name,
            'hardware_status': 'NOT_PROBED',
        }, indent=2) + '\n')
        files = sorted(item for item in package.rglob('*') if item.is_file())
        (package / 'SHA256SUMS').write_text(''.join(
            f'{hashlib.sha256(item.read_bytes()).hexdigest()}  {item.relative_to(package).as_posix()}\n'
            for item in files))
        bundle = package / f'{OUTPUT_NAME}.zip'
        with zipfile.ZipFile(bundle, 'w', zipfile.ZIP_DEFLATED) as archive:
            for item in sorted(p for p in package.rglob('*') if p.is_file() and p != bundle):
                archive.write(item, item.relative_to(package))

        # Verify the final nested watchface bundle contains exactly the installer payload.
        with zipfile.ZipFile(archive_path) as archive:
            if set(archive.namelist()) != {'main.lua', f'resource-hook-{TARGET}.bin',
                                          f'resource-hook-{TARGET}.cmi.bin'}:
                raise ValueError('unexpected watchface archive contents')
            if archive.read(f'resource-hook-{TARGET}.bin') != module.read_bytes():
                raise ValueError('watchface ZIP module differs from built experimental ELF')
            if archive.read(f'resource-hook-{TARGET}.cmi.bin') != receipt.read_bytes():
                raise ValueError('watchface ZIP receipt differs from signed receipt')

        if output.exists() or output.is_symlink():
            raise RuntimeError(f'output appeared during build; refusing to replace: {output}')
        package.rename(output)

    print(f'Complete signed watchface package: {output}')
    print(f'Watchface input ZIP: {output}/xiaomi-band-11/build/resource-hook-prod.zip')
    print(f'Experimental ELF SHA-256: {hashlib.sha256((output / "xiaomi-band-11" / f"resource-hook-{TARGET}.bin").read_bytes()).hexdigest()}')
    print(f'Supervisor-matched AstroBox signing key SHA-256: {public_fingerprint}')
    print('Private key excluded. Hardware acceptance: NOT_PROBED.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        sys.exit(str(error))
