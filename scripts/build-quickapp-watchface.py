#!/usr/bin/env python3
"""Build normal Resource Hook installers using unsigned Canopus CMI2 receipts."""
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
import tomllib
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CANOPUS = ROOT.parent / 'Canopus-Private'
CANOPUS = Path(os.environ.get('CANOPUS_ROOT', DEFAULT_CANOPUS)).resolve()
WATCHFACE_BUILDER = CANOPUS / 'scripts/build_module_installer_prod.py'

TARGET_GROUPS = {
    'band11': ['xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155'],
    'band10pro': ['xiaomi-band-10-pro-3.101.043'],
}
ALL_TARGETS = [target for targets in TARGET_GROUPS.values() for target in targets] + ['xiaomi-band-11-4.100.177']
INSTALLER = runpy.run_path(str(ROOT / 'scripts/build-watchface.py'))
verify_watchface = INSTALLER['verify_watchface']


def build_bundle(device_family, targets, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f'rh-{device_family}-staging-') as tmp:
        stage = Path(tmp)
        payload_dir = stage / 'payload'
        payload_dir.mkdir(parents=True)
        manifest_data = tomllib.loads((ROOT / 'Canopus.toml').read_text())
        module_version = manifest_data['module']['version']

        target_metadata = {}
        modules, receipts = {}, {}
        for target in targets:
            target_payload = payload_dir / target
            env = INSTALLER['target_build_environment'](target, os.environ)
            env['CANOPUS_ROOT'] = str(CANOPUS)
            subprocess.run(['sh', str(ROOT / 'scripts/build-install-payload.sh'),
                            target, str(target_payload)], cwd=ROOT, env=env, check=True)
            elf_dest = target_payload / 'resource-hook.elf'
            receipt = target_payload / 'receipt.bin'
            fw_toml = CANOPUS / 'targets' / target / 'target.toml'
            fw_sha = tomllib.loads(fw_toml.read_text())['firmware_sha256']

            modules[target] = elf_dest
            receipts[target] = receipt
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

        verify_watchface(generated_device, targets, modules, receipts,
                         archive_path=generated_prod_zip)
        manifest_path = generated_device / 'build/manifest.json'
        installer_manifest = json.loads(manifest_path.read_text())
        font_status = ('NOT_PROBED' if 'xiaomi-band-11-4.100.177' in targets
                       else 'USER_REPORTED_PASS')
        installer_manifest.update({
            'module_build_id': 'resource-hook-0.3.0', 'font_reload': True,
            'font_reload_device_status': font_status,
            'receipt_format_version': 2,
            'hardware_status': 'NOT_PROBED', 'gpu_recovery': 'UNSUPPORTED',
            'framework_restart_safety': 'UNSUPPORTED',
        })
        manifest_path.write_text(json.dumps(installer_manifest, indent=2) + '\n')

        # Destination paths
        if device_family == 'band11':
            versions = '-'.join(target.rsplit('.', 1)[1] for target in targets)
            folder_name = 'resource-hook-0.3.0-quickapp-band11-' + versions
            zip_name = folder_name + '.zip'
        else:
            folder_name = 'resource-hook-0.3.0-quickapp-band10-pro-1043'
            zip_name = folder_name + '.zip'

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
        (dest_folder / 'docs').mkdir()
        shutil.copyfile(ROOT / 'docs/FONT_RELOAD.md', dest_folder / 'docs/FONT_RELOAD.md')
        for target in targets:
            evidence = dest_folder / 'evidence' / target
            evidence.mkdir(parents=True)
            for item in (ROOT / 'targets' / target).iterdir():
                if item.is_file() and item.suffix in ('.json', '.md'):
                    shutil.copyfile(item, evidence / item.name)
        verify_watchface(dest_folder / 'watchface' / device_dir_name,
                         targets, modules, receipts, archive_path=dest_zip)

        flat_files = ['main.lua']
        for target in targets:
            flat_files.append(f'resource-hook-{target}.bin')
            flat_files.append(f'resource-hook-{target}.cmi.bin')

        validation = {
            'module': 'corona',
            'version': module_version,
            'features': ['transactional-font-reload', 'quickapp-launcher-icons', 'calendar-reload'],
            'module_build_id': 'resource-hook-0.3.0',
            'font_reload': True, 'font_reload_device_status': font_status,
            'gpu_recovery': 'UNSUPPORTED', 'framework_restart_safety': 'UNSUPPORTED',
            'targets': target_metadata,
            'receipt_format_version': 2,
            'enabled_on_install': False,
            'device_status': 'NOT_PROBED',
            'flat_zip_files': sorted(flat_files),
            'checks': [
                'strict ELF verification for targets',
                'CMI2 target/fingerprint/ELF binding',
                'production installer generation',
                'flat ZIP byte equality, CRC and exact resource allowlist',
            ]
        }
        (dest_folder / 'validation.json').write_text(json.dumps(validation, indent=2) + '\n')

        readme_text = (
            f"# Resource Hook installer: {device_family.upper()}\n\n"
            f"Targets: {', '.join(targets)}. Runtime module: corona {module_version}.\n\n"
            "The flat ZIP is input to a watchface packer, not a vendor watchface file.\n"
            "Opening the installed watchface installs the module DISABLED. Enable it separately.\n"
            "Requires the current resident Canopus Supervisor with unsigned CMI2 support.\n"
            "Includes transactional font reload, QuickApp launcher icon routing (@quickapp-icon/<package>) and calendar reload.\n"
            f"Font reload: {font_status} (legacy targets have user reports). This new artifact is NOT_PROBED on hardware.\n"
            "GPU recovery and framework restart safety are unsupported; native allocation/file-write success is not guaranteed.\n"
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
    parser.add_argument('--target', choices=ALL_TARGETS, action='append',
                        help='select exact targets instead of family defaults; repeat for a bundle')
    parser.add_argument('--family', choices=['band11', 'band10pro', 'all'], default='band11',
                        help='Device family to package (default: band11)')
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'dist',
                        help='Output directory under which watchface packages are saved')
    args = parser.parse_args()

    targets = args.target or [target for family, values in TARGET_GROUPS.items()
                              if args.family in ('all', family) for target in values]
    if len(set(targets)) != len(targets):
        parser.error('duplicate target')
    try:
        INSTALLER['build_options'](targets, os.environ)
    except ValueError as error:
        parser.error(str(error))
    groups = INSTALLER['device_groups'](targets)
    for device, selected in groups.items():
        family = 'band11' if device == 'xiaomi-band-11' else 'band10pro'
        print(f"Building watchface bundle for {family} ({', '.join(selected)})...")
        build_bundle(family, selected, args.output_dir.resolve())


if __name__ == '__main__':
    main()
