#!/usr/bin/env python3
"""Add Resource Hook's exact-target flags to the Canopus C build pipeline."""
import os
from pathlib import Path
import subprocess
import sys

TARGET_FLAGS = {
    'xiaomi-band-11-4.100.139': [],
    'xiaomi-band-11-4.100.155': ['-DRH_TARGET_155=1'],
    'xiaomi-band-11-4.100.177': ['-DRH_TARGET_177=1'],
    'xiaomi-band-10-pro-3.101.043': ['-DRH_TARGET_1043=1'],
}


def main():
    args = sys.argv[1:]
    targets = {
        Path(arg[2:]).parent.name
        for arg in args
        if arg.startswith('-I') and Path(arg[2:]).name == 'generated'
    }
    if len(targets) != 1 or next(iter(targets)) not in TARGET_FLAGS:
        sys.exit('Resource Hook requires one supported exact Canopus target include directory')
    target = targets.pop()
    flags = list(TARGET_FLAGS[target])
    for name in ('RH_P67_LIVE_LAUNCHER', 'RH_P67_LIVE_BACKGROUNDS', 'RH_P67_BLUR_NATIVE'):
        if name in os.environ:
            sys.exit(f'Retired build flag {name}; remove it')
    if os.environ.get('RH_P67_BLUR_DIAGNOSTICS', '0') == '1':
        if target != 'xiaomi-band-10-pro-3.101.043':
            sys.exit('Blur diagnostics requires exact .043 target')
        flags.append('-DRH_P67_BLUR_DIAGNOSTICS=1')
    return subprocess.call([os.environ.get('CLANG', 'clang'), *flags, *args])


if __name__ == '__main__':
    sys.exit(main())
