"""Exact .155 -> .139 font-audit identities and reproducible binary verification.

The JSON is an explicit per-address audit, never a range relocation. The CLI
requires both exact APs, and optionally revalidates the supplied OTA and loader.
Neither IDA nor the adjacent Canopus checkout is needed for static verification.
"""
import argparse
import ast
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / 'targets/xiaomi-band-11-4.100.139/font-reload-compatibility.json'
AUDIT = json.loads(EVIDENCE.read_text())
ADDRESS_MAP = {int(k, 16): int(v, 16)
               for k, v in AUDIT['address_map_155_to_139'].items()}
TARGETS = ('xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155')


def native(address, target=None):
    """Resolve one .155 audit identity, preserving only code Thumb bits."""
    target = target or os.environ.get('RESOURCE_HOOK_TARGET', TARGETS[0])
    if target not in TARGETS:
        raise RuntimeError(f'unsupported font-probe target: {target!r}')
    thumb = address & 1 if 0x0c000000 <= address < 0x0d000000 else 0
    key = address & ~thumb
    if key not in ADDRESS_MAP:
        raise RuntimeError(f'unverified font-probe address: {address:#010x}')
    return (ADDRESS_MAP[key] if target == TARGETS[0] else key) | thumb


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def validate_direct_branch(old_dest, new_dest, old_start, new_start, size,
                           *, allow_boundary=False):
    """Return False only for an explicitly permitted, unverified boundary.

    A changed-encoding branch in an original audited body must never permit
    this boundary. Added one-hop bodies do not trigger recursive expansion.
    """
    if old_start <= old_dest < old_start + size:
        require(old_dest - old_start == new_dest - new_start,
                'internal branch topology changed')
        return True
    if old_dest in ADDRESS_MAP:
        require(ADDRESS_MAP[old_dest] == new_dest, 'branch mapping differs')
        code_evidence = {int(row['address_155'], 16)
                         for group in ('functions', 'direct_dependency_proofs')
                         for row in AUDIT[group]}
        require(old_dest in code_evidence, 'mapped branch target lacks code evidence')
        return True
    require(allow_boundary, f'missing branch mapping for {old_dest:#x}')
    return False


def verify_independent_anchor(row, images):
    """Re-find a target from a unique exact-byte anchor, not a presumed delta."""
    anchor = row['independent_anchor']
    needle = bytes.fromhex(anchor['bytes'])
    require(len(needle) >= 8, 'independent anchor too short')
    relative = anchor['offset_from_entry']
    if anchor['kind'] == 'within_function':
        require(0 <= relative and relative + len(needle) <= row['size'],
                'independent anchor is outside its claimed function')
    else:
        require(anchor['kind'] == 'context_window' and
                -64 <= relative and relative + len(needle) <= row['size'] + 64,
                'independent anchor exceeds bounded context window')
    for target, image in images.items():
        offset = image.find(needle)
        require(offset >= 0 and image.find(needle, offset + 1) < 0,
                f'.{target} independent anchor missing or ambiguous')
        address = 0x0c0c0000 + offset
        require(address == int(anchor[f'address_{target}'], 16),
                'independent anchor location differs')
        require(address - anchor['offset_from_entry'] == int(row[f'address_{target}'], 16),
                'independent anchor does not locate mapped target')
    require(ADDRESS_MAP.get(int(row['address_155'], 16)) == int(row['address_139'], 16),
            'independent target proof contradicts or lacks address mapping')


def loader_layout(loader, data):
    # Execute the supplied loader's actual pure parser, without importing IDA.
    # Its load_file/segment-creation functions remain defined but are not called.
    tree = ast.parse(loader.read_text())
    tree.body = [node for node in tree.body if not (
        isinstance(node, ast.Import) and
        any(alias.name.startswith('ida_') for alias in node.names))]
    namespace = {}
    exec(compile(tree, str(loader), 'exec'), namespace)

    class ImageInput(io.BytesIO):
        def size(self):
            return len(self.getvalue())

    layout = namespace['_probe'](ImageInput(data))
    require(layout and layout.has_aliases and layout.has_ram_layout,
            'loader did not validate flash aliases and RAM copy layout')
    return {key: hex(value) if isinstance(value, int) else value
            for key, value in vars(layout).items()}


def verify(ap139, ap155, container=None, loader=None):
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
    from capstone.arm import ARM_OP_MEM, ARM_REG_PC

    images = {'139': ap139.read_bytes(), '155': ap155.read_bytes()}
    identity = AUDIT['identity']
    for target, data in images.items():
        require(sha(data) == identity[f'ap{target}_sha256'],
                f'.{target} AP fingerprint mismatch')
    if container:
        require(sha(container.read_bytes()) == identity['container_sha256'],
                'OTA container fingerprint mismatch')
        with zipfile.ZipFile(container) as archive:
            require(archive.read(identity['member']) == images['139'],
                    'OTA vela_ap.bin is not byte-identical to analysis AP')
    if loader:
        require(sha(loader.read_bytes()) == identity['loader_sha256'],
                'loader fingerprint mismatch')
        for target, data in images.items():
            require(loader_layout(loader, data) == identity['layouts'][target],
                    f'.{target} loader layout differs')

    def read(target, address, size):
        base = 0x2c0c0000 if address >= 0x2c000000 else 0x0c0c0000
        offset = address - base
        require(0 <= offset <= len(images[target]) - size, 'flash read out of bounds')
        return images[target][offset:offset + size]

    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
    md.detail = True
    functions = AUDIT['functions']
    dependency_proofs = AUDIT['direct_dependency_proofs']
    for proof in dependency_proofs:
        verify_independent_anchor(proof, images)
    instruction_count = 0
    dependency_instruction_count = 0
    boundary_count = 0
    for row in functions + dependency_proofs:
        added_layer = 'independent_anchor' in row
        address = {t: int(row[f'address_{t}'], 16) for t in images}
        blocks = {t: read(t, address[t], row['size']) for t in images}
        for t, block in blocks.items():
            require(sha(block) == row[f'sha256_{t}'],
                    f'.{t} function bytes differ at {address[t]:#x}')
        require((blocks['139'] == blocks['155']) == row['exact_bytes'],
                'incorrect exact-body equality claim')
        differences, literals, unverified_dependencies = [], [], []
        decoded = 0
        count = 0
        for lo, hi in row['code_spans']:
            dis = {t: list(md.disasm(blocks[t][lo:hi], address[t] + lo)) for t in images}
            require(len(dis['139']) == len(dis['155']), 'instruction count changed')
            require(sum(i.size for i in dis['155']) == hi - lo, 'incomplete disassembly')
            require(sum(i.size for i in dis['139']) == hi - lo, 'incomplete .139 disassembly')
            for old, new in zip(dis['155'], dis['139']):
                decoded += old.size
                count += 1
                require(old.size == new.size and old.mnemonic == new.mnemonic,
                        f'instruction shape changed at {old.address:#x}')
                if old.bytes != new.bytes:
                    # Full register/offset/immediate layouts must be unchanged;
                    # only direct control-transfer destinations may differ.
                    require(old.mnemonic.startswith('b') and
                            old.op_str.startswith('#') and new.op_str.startswith('#'),
                            f'non-branch instruction changed at {old.address:#x}')
                    differences.append([hex(old.address), old.mnemonic + ' ' + old.op_str,
                                        hex(new.address), new.mnemonic + ' ' + new.op_str])
                else:
                    # PC-relative loads share encoding; their literal values
                    # are verified independently, not mistaken for same data.
                    require(old.op_str == new.op_str or old.mnemonic.startswith('b') or
                            old.mnemonic in ('cbz', 'cbnz', 'adr', 'adr.w'),
                            'unexplained PC-relative operand difference')
                if old.mnemonic.startswith('b') and old.op_str.startswith('#'):
                    old_dest, new_dest = int(old.op_str[1:], 0), int(new.op_str[1:], 0)
                    verified = validate_direct_branch(
                        old_dest, new_dest, address['155'], address['139'], row['size'],
                        allow_boundary=added_layer or old.bytes == new.bytes)
                    if not verified:
                        unverified_dependencies.append(dict(
                            pc_155=hex(old.address), pc_139=hex(new.address),
                            target_155=hex(old_dest), target_139=hex(new_dest),
                            instruction_bytes_changed=old.bytes != new.bytes,
                            status='UNVERIFIED_TRANSITIVE_DEPENDENCY',
                            basis='decoded destination pair only; no target-body equivalence asserted'))
                for operand in old.operands:
                    if operand.type == ARM_OP_MEM and operand.mem.base == ARM_REG_PC:
                        old_lit = ((old.address + 4) & ~3) + operand.mem.disp
                        new_lit = old_lit + address['139'] - address['155']
                        old_val = struct.unpack('<I', read('155', old_lit, 4))[0]
                        new_val = struct.unpack('<I', read('139', new_lit, 4))[0]
                        literals.append(dict(pc_155=hex(old.address), pc_139=hex(new.address),
                                             address_155=hex(old_lit), address_139=hex(new_lit),
                                             value_155=hex(old_val), value_139=hex(new_val)))
                        key = old_val & ~1 if 0x0c000000 <= old_val < 0x0d000000 else old_val
                        if key in ADDRESS_MAP:
                            require(native(old_val, TARGETS[0]) == new_val,
                                    'literal contradicts explicit mapping')
                        if 0x20000000 <= old_val < 0x20200000:
                            require(old_val == new_val, 'RAM literal changed')
        # IDA's inline data pools must not be treated as instructions. Every
        # changed pool byte must be covered by a decoded PC-relative reference.
        referenced = {byte for literal in literals
                      for byte in range(int(literal['address_155'], 16),
                                        int(literal['address_155'], 16) + 4)}
        coverage = []
        for lo, hi in row['noncode_spans']:
            coverage.extend(range(lo, hi))
            for offset in range(lo, hi):
                if blocks['155'][offset] != blocks['139'][offset]:
                    require(address['155'] + offset in referenced,
                            'unexplained inline-data byte difference')
        for lo, hi in row['code_spans']:
            coverage.extend(range(lo, hi))
        require(sorted(coverage) == list(range(row['size'])),
                'function bytes missing or overlapping in instruction/data partition')
        require(differences == row['differences'], 'instruction difference receipt changed')
        require(literals == row['literals'], 'PC literal receipt changed')
        require(count == row['instructions'] and decoded == row['decoded_bytes'],
                'instruction coverage receipt changed')
        require(unverified_dependencies == row['unverified_transitive_dependencies'],
                'unverified dependency boundary receipt differs')
        boundary_count += len(unverified_dependencies)
        if added_layer:
            dependency_instruction_count += count
        else:
            instruction_count += count
    require(boundary_count == AUDIT['direct_dependency_scope']['unverified_transitive_callsite_count'],
            'unverified dependency boundary count differs')
    for row in AUDIT['data']:
        for t in images:
            data = read(t, int(row[f'address_{t}'], 16), row['size'])
            require(sha(data) == row[f'sha256_{t}'], 'class/default-font data changed')
            require([hex(w) for w in struct.unpack('<' + 'I' * (len(data) // 4), data)] ==
                    row[f'words_{t}'], 'class/default-font words differ')
    for copy in AUDIT['startup_code_copies']:
        offset, destination, size = (int(copy[k], 16) for k in ('offset', 'destination', 'size'))
        for t, image in images.items():
            triples = [struct.unpack_from('<I', image, int(p, 16))[0]
                       for p in copy['startup_literal_offsets']]
            require(triples == [0x2c0c0000 + offset, destination, destination + size],
                    'startup copy literal triple differs')
            require(sha(image[offset:offset + size]) == copy[f'sha256_{t}'],
                    'startup code bytes differ')
    for address in AUDIT['source_contract']['address_literals']:
        native(int(address, 16), TARGETS[0])
        native(int(address, 16), TARGETS[1])
    print(f'PASS {len(functions)} function bodies / {instruction_count} instructions, '
          f'{len(AUDIT["data"])} data records, '
          f'{len(AUDIT["source_contract"]["address_literals"])} source address identities')
    print(f'PASS {len(dependency_proofs)} independently located direct targets / '
          f'{dependency_instruction_count} additional local instructions')
    print(f'LIMITED: {boundary_count} explicitly unverified transitive callsites; '
          'one assertion target has entry-block-only coverage. '
          'No whole-program semantic-equivalence claim.')
    if container:
        print('PASS supplied OTA member == .139 analysis AP (exact bytes)')
    if loader:
        print('PASS supplied loader parser: both flash aliases / RAM / BSS layouts')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ap139', type=Path, default=ROOT / 'build/firmware-analysis/vela_ap_4.100.139.bin')
    parser.add_argument('--ap155', type=Path, default=ROOT / 'build/firmware-analysis/vela_ap_4.100.155.bin')
    parser.add_argument('--container', type=Path)
    parser.add_argument('--loader', type=Path)
    args = parser.parse_args()
    verify(args.ap139, args.ap155, args.container, args.loader)


if __name__ == '__main__':
    main()
