"""Exact-target setup for resource-hook firmware tests.

RESOURCE_HOOK_TARGET defaults to .139. Unknown targets and unmapped addresses
raise errors (never SkipTest): a green .155 run must not actually execute .139.
The bootstrap harness remaps its own hooks, NOT callers' PCs or RAM globals.
"""
import hashlib
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_TARGET = 'xiaomi-band-11-4.100.139'
PORT_TARGET = 'xiaomi-band-11-4.100.155'
FIRMWARE_SHA256 = {
    SOURCE_TARGET: '31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74',
    PORT_TARGET: 'ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f',
}
TARGET = os.environ.get('RESOURCE_HOOK_TARGET', SOURCE_TARGET)
if TARGET not in (SOURCE_TARGET, PORT_TARGET):
    raise RuntimeError(f'unsupported RESOURCE_HOOK_TARGET: {TARGET!r}')

# Preserve the public checkout default, but support the adjacent private tree.
_default_root = ROOT.parent / 'Canopus'
if not _default_root.is_dir():
    _default_root = ROOT.parent / 'Canopus-Private'
CANOPUS = Path(os.environ.get('CANOPUS_ROOT', _default_root)).resolve()
sys.path.insert(0, str(CANOPUS / 'scripts/tests'))

# Explicit .155 identities, recovered from the exact AP's LVGL accessors and
# their call sites. These are NOT a range relocation rule. The platform audit
# records the AP SHA256 ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f.
# Display accessor/invalidation decompilation: platform .155 address evidence;
# POSIX open: Canopus targets/<target>/evidence/fw-match/port-139-to-155.json.
_ADDRESSES_155 = {
    0x0c33dc4e: 0x0c33dc4e,  # native write, framework emulation-addresses.json
    0x0c342c54: 0x0c342c54,  # native open called by POSIX open
    0x0c380694: 0x0c380694,  # horizontal resolution (rotation aware)
    0x0c3806b4: 0x0c3806b4,  # vertical resolution (rotation aware)
    0x0c3807bc: 0x0c3807bc,  # DPI: .155 80b500af10b9034b986908b1806980bd
    0x0c3807ec: 0x0c3807ec,  # active screen at display +696
    0x0c3809a4: 0x0c3809a4,  # event dispatch called by invalidation
    0x0c382428: 0x0c382428,  # _lv_inv_area
    0x0c3a4360: 0x0c3a4360,  # driver lookup: LDRD [0x200bd1e8 + 0x1c4]
    0x0c3a6194: 0x0c3a6194,  # LVGL POSIX open
    0x0c3abd20: 0x0c3abd20,  # timer create, exact-target port evidence
    0x0c3abe70: 0x0c3abe70,  # timer delete, exact-target port evidence
    0x200bd200: 0x200bd200,  # default display, accessor literal
    0x200bd310: 0x200bd310,  # decoded cache, lv_init stores
    0x200bd314: 0x200bd314,  # header cache, lv_init stores
    0x200bd3ac: 0x200bd3ac,  # driver list node size, driver lookup literal
    0x200bd3b0: 0x200bd3b0,  # driver list head, driver lookup literal
    0x200bd3b8: 0x200bd3b8,  # POSIX driver, lv_init stores
    0x200bd3bc: 0x200bd3bc,  # driver buffer size, lv_init stores
    0x200bd3c4: 0x200bd3c4,  # driver open callback, lv_init stores
    0x2ca168c4: 0x2ca168b4,  # decoded IMAGE class, lv_init literal
    0x2ca16944: 0x2ca16934,  # IMAGE_HEADER class, distinct lv_init literal
    # Exact .155 instruction/call-site audit (Capstone 5, fingerprint above):
    # drop-all 0xc3a7918..0xc3a79c4, cache release 0xc8b9780..0xc8b97d8;
    # font eviction 0xc4940fc..0xc494174, wrapper delete 0xc917384..0xc91745a;
    # page create/resume/destroy 0xc696a6c..0xc696f0e. Each entry below is
    # corroborated by those bodies/vtable literals, NOT inferred from a delta.
    0x0c378b1e: 0x0c378b1e,  # reset call at 0xc3a7956
    0x0c3a3888: 0x0c3a3888,  # both-cache dispatch (framework port evidence)
    0x0c3a7918: 0x0c3a7918,  # referenced-node payload/node frees remain unsafe
    0x0c3a5e34: 0x0c3a5e34,  # logging calls in all audited bodies
    0x0c3abe58: 0x0c3abe58,  # frees at 0xc3a79ba/0xc3a79c0
    0x0c3a4ace: 0x0c3a4ace,  # class +12 lookup in .155 IMAGE vtable
    0x0c3a472c: 0x0c3a472c,  # class +20 unlink in .155 IMAGE vtable
    0x0c8b9790: 0x0c8b9780,  # decrements +4; frees only zero-ref invalid entry
    0x0c8b8cae: 0x0c8b8c9e,  # cache drop called by font eviction at 0xc49414c
    0x0c39a424: 0x0c39a424,  # release pathname at 0xc494154
    0x0c3a46dc: 0x0c3a46dc,  # list unlink at 0xc49416a
    0x0c3a43b0: 0x0c3a43b0,  # list allocation at 0xc4943f0/0xc917432
    0x0c3a4a6a: 0x0c3a4a6a,  # idle count at 0xc91741e
    0x0c4940fc: 0x0c4940fc,  # idle-font eviction
    0x0c494380: 0x0c494380,  # active/idle font reuse precedes resolution
    0x0c917394: 0x0c917384,  # wrapper release -> idle cache
    0x0c490edc: 0x0c490edc,  # registry resolver, call at 0xc494502
    0x0c34f8ec: 0x0c34f8ec,  # native access at 0xc49450a
    0x0c4924e0: 0x0c4924e0,  # registry append
    0x0c904cfc: 0x0c904cec,  # registry remove; platform target header
    0x0c3abe20: 0x0c3abe20,  # allocator at 0xc49253e/0xc492558
    0x0c3ac2bc: 0x0c3ac2bc,  # realloc body 0xc3ac2bc..0xc3ac2fa
    0x200bd1e8: 0x200bd1e8,  # registry global in append/remove/resolver
    0x200bd3ec: 0x200bd3ec,  # eviction: literal 0x200bd1e8 + 0x204
    0x0c696e34: 0x0c696e24,  # forced page pop; platform target header
    0x0c696eb8: 0x0c696ea8,  # with-policy pop, dispatch to audited ladder
    0x0c696c18: 0x0c696c08,  # resume; platform target header
    0x0c350474: 0x0c350474,  # page logging at 0xc696d1e
    0x0c6d29f8: 0x0c6d29e8,  # lifecycle trace at 0xc696a7a
    0x0c384e6c: 0x0c384e6c,  # root deletion at 0xc696aa2
    0x0c6acd04: 0x0c6accf4,  # root allocation at 0xc696b38
    0x0c3840d8: 0x0c3840d8,  # root flags at 0xc696ca4
    0x0c37fea0: 0x0c37fea0,  # event removal at 0xc696d40
    0x0c695d4c: 0x0c695d3c,  # create precondition at 0xc696b18
    0x0c387ba8: 0x0c387ba8,  # root size helper at 0xc696b44
    0x0c9195ba: 0x0c9195aa,  # assertion at 0xc696b28
    0x0c4fbb10: 0x0c4fbb10,  # create helper at 0xc696b6a
    0x0c383278: 0x0c383278,  # create helper at 0xc696b70
    0x0c8b9462: 0x0c8b9452,  # create helper at 0xc696b78
    0x0c6b90d0: 0x0c6b90c0,  # resume helper at 0xc696cb4
    0x0c37f8f0: 0x0c37f8f0,  # create helper at 0xc696b9c
    0x0c385038: 0x0c385038,  # create helper at 0xc696ba4
    0x200c2a28: 0x200c2a28,  # screen state read in audited resume
    0x20096085: 0x20096085,  # page layer read in audited resume
    0x200c33bc: 0x200c33bc,  # root-parent literal at 0xc696be4
    # Restart counterexamples: exact .155 loader 0xc330720..0xc330764,
    # task_start 0xc35a344..0xc35a3d0, group_leave 0xc3533f4..0xc353646,
    # uikit teardown 0xc494198..0xc4942ae, main 0xc7def28..0xc7defa4,
    # font-init 0xc7e88a0..0xc7e894a and completion 0xc7ea45c..0xc7ea4c2.
    0x0c330720: 0x0c330720,  # builtin loader
    0x0c34938c: 0x0c34938c,  # basename search, call at 0xc33072c
    0x0c34834c: 0x0c34834c,  # builtin lookup, call at 0xc330736
    0x2ca02564: 0x2ca02554,  # literal at 0xc330764; slot 30 is miwear
    0x0c7def88: 0x0c7def78,  # slot 30 entry = 0xc7def79
    0x0c35a344: 0x0c35a344,  # TCB argv/entry dispatch
    0x0c34ee38: 0x0c34ee38,  # exit leaf, call at 0xc35a3b4
    0x200b04bc: 0x200b04bc,  # current TCB, literal at 0xc35a3c4
    0x200b98e4: 0x200b98e4,  # constructor guard, literal at 0xc35a3d0
    0x0c3533f4: 0x0c3533f4,  # group_leave clears tcb+8, cleans group
    0x0c34d9c0: 0x0c34d9c0,  # group-object free at 0xc3534b2/0xc3534ea
    0x0c34da3c: 0x0c34da3c,  # stdio teardown at 0xc353478
    0x0c34da52: 0x0c34da52,  # lock teardown at 0xc353480/0xc353488
    0x0c334998: 0x0c334998,  # file cleanup at 0xc3535b8
    0x0c494198: 0x0c494198,  # UIkit teardown with live-font warning
    0x0c3a9f40: 0x0c3a9f40,  # LVGL deinit at 0xc49423c
    0x0c4948a0: 0x0c4948a0,  # font manager init at 0xc7e88f6
    0x0c4ffc90: 0x0c4ffc90,  # font file copy at 0xc7e88fe/0xc7e8916
    0x0c7e88b0: 0x0c7e88a0,  # initialization callback, pointer at 0xc7def70
    0x0c7e895a: 0x0c7e894a,  # exact stop PC after guarded font registration
    0x200da910: 0x200da910,  # init state literals at 0xc7def60/0xc7e8a74
    0x200da864: 0x200da864,  # graphics init progress, literal at 0xc7e8af8
    0x200da7f8: 0x200da7f8,  # font one-time guard, literal at 0xc7e8a84
    0x0c3f5064: 0x0c3f5064,  # loop allocation at 0xc7def7c
    0x0c3f0934: 0x0c3f0934,  # timer init at 0xc7def86
    0x0c3eab0c: 0x0c3eab0c,  # loop run at 0xc7def92
    0x0c3f0a6e: 0x0c3f0a6e,  # timer scheduling at 0xc7def56
    0x0c3f0968: 0x0c3f0968,  # timer stop at 0xc7ea45e
    0x0c6d0fd4: 0x0c6d0fc4,  # booting-completed notification at 0xc7ea470
    0x0c4fbbc4: 0x0c4fbbc4,  # vibration guard at 0xc7ea480
    0x0c8ec158: 0x0c8ec148,  # completion helper at 0xc7ea4a2
}


def fw(address):
    """Translate one evidenced source address, preserving a Thumb function bit.

    Odd RAM addresses must have their own explicit entry; never mask a RAM byte
    address as though it were a Thumb pointer.
    """
    if TARGET == SOURCE_TARGET:
        return address
    thumb = address & 1 if 0x0c000000 <= address < 0x0d000000 else 0
    source = address & ~thumb
    try:
        return _ADDRESSES_155[source] | thumb
    except KeyError:
        raise RuntimeError(
            f'{TARGET}: unsupported firmware test address {address:#010x}; '
            'exact-target mapping/evidence is required (no .139 fallback)'
        ) from None


def require_identity_addresses(*addresses):
    """Assert literal PCs/globals retained by a fixture are exact identities."""
    for address in addresses:
        actual = fw(address)
        if actual != address:
            raise RuntimeError(
                f'{TARGET}: fixture must explicitly translate {address:#x} to {actual:#x}')


def check_firmware():
    firmware = CANOPUS / 'fwbins' / TARGET / 'vela_ap.bin'
    if not firmware.is_file():
        raise RuntimeError(f'missing selected-target firmware: {firmware}')
    digest = hashlib.sha256(firmware.read_bytes()).hexdigest()
    if digest != FIRMWARE_SHA256[TARGET]:
        raise RuntimeError(f'{firmware}: firmware fingerprint mismatch for {TARGET}: {digest}')


def Machine(*args, **kwargs):
    """Always pass the selected target; never inherit bootstrap's .139 default."""
    requested = kwargs.pop('target', TARGET)
    if requested != TARGET:
        raise RuntimeError(f'Machine target {requested!r} differs from {TARGET!r}')
    check_firmware()
    from band11_arm_bootstrap import Machine as BootstrapMachine
    return BootstrapMachine(*args, target=TARGET, **kwargs)


def hook(machine, source, callback, *, synthetic=False):
    """Replace a source-keyed hook; synthetic PSRAM callbacks are not firmware."""
    if synthetic and not 0x1c000000 <= source < 0x1d000000:
        raise ValueError('synthetic hooks must use the modeled PSRAM code arena')
    actual = source if synthetic else fw(source)
    if source in machine.firmware_hooks:
        machine.uc.hook_del(machine.firmware_hooks.pop(source))
    from unicorn import UC_HOOK_CODE
    machine.firmware_hooks[source] = machine.uc.hook_add(
        UC_HOOK_CODE, machine.firmware_call, callback, actual, actual)
