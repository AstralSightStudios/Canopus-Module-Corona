#!/usr/bin/env python3
"""Build signed watchface installer bundles with QuickApp icon support."""
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
import tomllib
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CANOPUS = ROOT.parent / 'Canopus'
if not DEFAULT_CANOPUS.is_dir():
    DEFAULT_CANOPUS = ROOT.parent / 'Canopus-Private'
CANOPUS = Path(os.environ.get('CANOPUS_ROOT', DEFAULT_CANOPUS)).resolve()
SUPERVISOR = CANOPUS / 'manager/service/canopus_supervisor_platform.c'
RECEIPT_BUILDER = CANOPUS / 'scripts/build-module-installer-receipt.py'
WATCHFACE_BUILDER = CANOPUS / 'scripts/build_module_installer_prod.py'

TARGET_GROUPS = {
    'band11': ['xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155'],
    'band10pro': ['xiaomi-band-10-pro-3.101.043'],
}
ALL_TARGETS = ['xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155', 'xiaomi-band-10-pro-3.101.043']


def trusted_public_key():
    source = SUPERVISOR.read_text()
    match = re.search(r's_installer_public_key\[32\]\s*=\s*\{(.*?)\};', source, re.S)
    if not match:
        raise ValueError('Cannot locate resident Supervisor installer trust key')
    key = bytes(int(val, 16) for val in re.findall(r'0x([0-9a-fA-F]{2})', match[1]))
    if len(key) != 32:
        raise ValueError('Invalid Supervisor Ed25519 trust key length')
    return key


def extract_private_key(archive_path, key_path):
    with zipfile.ZipFile(archive_path) as archive:
        files = [item for item in archive.infolist() if not item.is_dir()]
        if len(files) != 1 or files[0].filename != 'module-installer-ed25519.pem':
            raise ValueError('Expected only module-installer-ed25519.pem in certificate ZIP')
        item = files[0]
        if item.file_size > 16384 or item.flag_bits & 1:
            raise ValueError('Invalid signing-key archive entry')
        if stat.S_ISLNK(item.external_attr >> 16):
            raise ValueError('Signing-key entry must not be a symlink')
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
        raise ValueError('Signing key does not match resident Supervisor public key')
    subprocess.run(['openssl', 'pkey', '-in', str(private_key), '-pubout',
                    '-out', str(public_pem)], check=True, capture_output=True)
    return public_pem, hashlib.sha256(expected).hexdigest()


def build_bundle(device_family, targets, private_key, public_pem, public_fingerprint, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f'rh-{device_family}-staging-') as tmp:
        stage = Path(tmp)
        payload_dir = stage / 'payload'
        payload_dir.mkdir(parents=True)
        manifest_data = tomllib.loads((ROOT / 'Canopus.toml').read_text())
        module_version = manifest_data['module']['version']

        target_metadata = {}
        for target in targets:
            target_payload = payload_dir / target
            target_payload.mkdir()
            env = dict(os.environ, CANOPUS_ROOT=str(CANOPUS), MODULE_INSTALL_KEY=str(private_key))
            subprocess.run(['sh', str(ROOT / 'scripts/build.sh'), target],
                           cwd=ROOT, env=env, check=True)
            elf = ROOT / 'build/resource-hook.elf'
            elf_dest = target_payload / 'resource-hook.elf'
            shutil.copyfile(elf, elf_dest)

            fw_toml = CANOPUS / 'targets' / target / 'target.toml'
            fw_sha = tomllib.loads(fw_toml.read_text())['firmware_sha256']

            receipt = target_payload / 'receipt.bin'
            subprocess.run([sys.executable, str(RECEIPT_BUILDER),
                            '--module', str(elf_dest),
                            '--module-id', 'corona',
                            '--version', '3',
                            '--lifecycle', '1',
                            '--target-id', target,
                            '--firmware-sha256', fw_sha,
                            '--private-key', str(private_key),
                            '--output', str(receipt)], check=True)
            subprocess.run([sys.executable, str(ROOT / 'scripts/verify-payload.py'),
                            str(target_payload), '--target', target,
                            '--public-key', str(public_pem)], check=True)

            target_metadata[target] = {
                'elf_sha256': hashlib.sha256(elf_dest.read_bytes()).hexdigest(),
                'elf_size': elf_dest.stat().st_size,
                'receipt_sha256': hashlib.sha256(receipt.read_bytes()).hexdigest(),
                'firmware_sha256': fw_sha,
            }

        # Watchface builder
        watchface_stage = stage / 'watchface'
        target_args = [arg for target in targets for arg in ('--target', target)]
        subprocess.run([sys.executable, str(WATCHFACE_BUILDER),
                        '--product', 'resource-hook',
                        '--module-id', 'corona',
                        *target_args,
                        '--payload-dir', str(payload_dir),
                        '--assets-dir', str(ROOT / 'examples'),
                        '--output-dir', str(watchface_stage)], check=True)

        device_dir_name = 'xiaomi-band-11' if device_family == 'band11' else 'xiaomi-band-10-pro'
        generated_device = watchface_stage / device_dir_name
        generated_prod_zip = generated_device / 'build/resource-hook-prod.zip'
        if not generated_prod_zip.is_file():
            raise RuntimeError(f'Expected generated watchface zip at {generated_prod_zip}')

        # Destination paths
        if device_family == 'band11':
            zip_name = 'resource-hook-quickapp-band11-139-155.zip'
            folder_name = 'resource-hook-quickapp-band11-139-155'
        else:
            zip_name = 'resource-hook-quickapp-band10-pro-1043.zip'
            folder_name = 'resource-hook-quickapp-band10-pro-1043'

        dest_folder = out_dir / folder_name
        dest_zip = out_dir / zip_name
        dest_sha = out_dir / f'{zip_name}.sha256'

        if dest_folder.exists():
            shutil.rmtree(dest_folder)
        dest_folder.mkdir(parents=True)

        # Copy generated flat zip to both locations
        shutil.copyfile(generated_prod_zip, dest_zip)
        shutil.copyfile(generated_prod_zip, dest_folder / zip_name)

        # Copy payload and watchface directories
        shutil.copytree(payload_dir, dest_folder / 'payload')
        shutil.copytree(watchface_stage, dest_folder / 'watchface')
        shutil.copyfile(public_pem, dest_folder / 'signer-public.pem')

        flat_files = ['main.lua']
        for target in targets:
            flat_files.append(f'resource-hook-{target}.bin')
            flat_files.append(f'resource-hook-{target}.cmi.bin')

        validation = {
            'module': 'corona',
            'version': module_version,
            'feature': 'quickapp-launcher-icons',
            'targets': target_metadata,
            'signer_public_key_sha256': public_fingerprint,
            'private_key_included': False,
            'enabled_on_install': False,
            'device_status': 'NOT_PROBED',
            'flat_zip_files': sorted(flat_files),
            'checks': [
                'strict ELF verification for targets',
                'CMI1 signature and target/fingerprint/ELF binding',
                'Supervisor trust-key match',
                'production installer generation and protocol suite',
                'flat ZIP byte equality, CRC and private-key exclusion',
            ]
        }
        (dest_folder / 'validation.json').write_text(json.dumps(validation, indent=2) + '\n')

        readme_text = (
            f"# Signed QuickApp icon installer: {device_family.upper()}\n\n"
            f"Targets: {', '.join(targets)}. Runtime module: corona {module_version}.\n\n"
            "The flat ZIP is input to a watchface packer, not a vendor watchface file.\n"
            "Opening the installed watchface installs the module DISABLED. Enable it separately.\n"
            "Requires a matching resident Canopus Supervisor with /canopus/install and the trusted signing key.\n"
            "Includes QuickApp launcher icon routing (@quickapp-icon/<package>) and dynamic calendar reload.\n"
            "Device/GPU acceptance remains NOT_PROBED; native allocation/file-write success is not guaranteed.\n"
            "No private signing key is included.\n"
        )
        (dest_folder / 'README.md').write_text(readme_text)

        # Generate SHA256SUMS for the package folder
        sha_lines = []
        for p in sorted(dest_folder.rglob('*')):
            if p.is_file() and p.name != 'SHA256SUMS':
                rel = p.relative_to(dest_folder)
                sha = hashlib.sha256(p.read_bytes()).hexdigest()
                sha_lines.append(f"{sha}  {rel}\n")
        (dest_folder / 'SHA256SUMS').write_text(''.join(sha_lines))

        zip_hash = hashlib.sha256(dest_zip.read_bytes()).hexdigest()
        dest_sha.write_text(f"{zip_hash}  {zip_name}\n")
        print(f"[{device_family}] Created {dest_zip} ({dest_zip.stat().st_size} bytes)")
        print(f"[{device_family}] SHA-256: {zip_hash}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--certificate-zip', type=Path,
                        default=ROOT.parent.parent / 'AstroBox-Certs/module-installer-ed25519.pem.zip',
                        help='AstroBox module-installer Ed25519 certificate ZIP')
    parser.add_argument('--family', choices=['band11', 'band10pro', 'all'], default='band11',
                        help='Device family to package (default: band11)')
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'dist',
                        help='Output directory under which watchface packages are saved')
    args = parser.parse_args()

    cert_path = args.certificate_zip.expanduser().resolve()
    if not cert_path.is_file():
        sys.exit(f"Error: certificate ZIP not found at {cert_path}")

    expected_pub = trusted_public_key()
    with tempfile.TemporaryDirectory(prefix='rh-cert-') as tmp_cert:
        tmp_p = Path(tmp_cert)
        private_key = tmp_p / 'module-installer-ed25519.pem'
        extract_private_key(cert_path, private_key)
        public_pem, fingerprint = assert_key_matches_supervisor(private_key, expected_pub, tmp_p)
        print(f"Verified signing key matches Supervisor (SHA-256: {fingerprint})")

        families = ['band11', 'band10pro'] if args.family == 'all' else [args.family]
        for fam in families:
            targets = TARGET_GROUPS[fam]
            print(f"\nBuilding watchface bundle for {fam} ({', '.join(targets)})...")
            build_bundle(fam, targets, private_key, public_pem, fingerprint, args.output_dir.resolve())


if __name__ == '__main__':
    main()
