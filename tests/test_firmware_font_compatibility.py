"""Fail-closed address/fingerprint guards for the dual-target native probes."""
import copy
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import firmware_font_compatibility as compatibility
from firmware_font_compatibility import native, TARGETS, verify, validate_direct_branch
from firmware_font_machine import Machine


class FontIdentityGuards(unittest.TestCase):
    def test_mapping_is_not_one_delta(self):
        source, port = TARGETS
        self.assertEqual(native(0x0c3abe20, source), 0x0c3abe20)
        self.assertEqual(native(0x0c8b9780, source), 0x0c8b9790)
        self.assertEqual(native(0x2ca16934, source), 0x2ca16944)
        self.assertEqual(native(0x200bd3ec, source), 0x200bd3ec)
        self.assertEqual(native(0x0c8b9780, port), 0x0c8b9780)

    def test_thumb_callback_bit_is_preserved(self):
        self.assertEqual(native(0x0c69feb1, TARGETS[0]), 0x0c69fec1)
        self.assertEqual(native(0x0c69feb1, TARGETS[1]), 0x0c69feb1)

    def test_unknown_identity_never_falls_back(self):
        for target in TARGETS:
            with self.assertRaisesRegex(RuntimeError, 'unverified'):
                native(0x0c123456, target)
            with self.assertRaisesRegex(RuntimeError, 'unverified'):
                native(0x200bd3ed, target)  # not a Thumb code pointer

    def test_unknown_target_never_defaults_to_139(self):
        with self.assertRaisesRegex(RuntimeError, 'unsupported'):
            native(0x0c3abe20, 'other-target')
        with patch.dict(os.environ, RESOURCE_HOOK_TARGET='other-target'):
            with self.assertRaisesRegex(RuntimeError, 'unsupported'):
                native(0x0c3abe20)

    def test_static_verifier_rejects_wrong_fingerprint(self):
        with tempfile.TemporaryDirectory() as directory:
            wrong = Path(directory) / 'wrong.bin'
            wrong.write_bytes(b'not the selected AP')
            with self.assertRaisesRegex(AssertionError, 'fingerprint mismatch'):
                verify(wrong, wrong)

    def test_native_machine_rejects_wrong_fingerprint_before_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            wrong = Path(directory) / 'wrong.bin'
            wrong.write_bytes(b'not the selected AP')
            with patch.dict(os.environ, RESOURCE_HOOK_FIRMWARE=str(wrong)):
                with self.assertRaisesRegex(AssertionError, 'wrong selected-target AP'):
                    Machine()


class DirectDependencyGuards(unittest.TestCase):
    """Exercise P2 mutations against the actual fingerprinted AP evidence."""
    ap139 = compatibility.ROOT / 'build/firmware-analysis/vela_ap_4.100.139.bin'
    ap155 = compatibility.ROOT / 'build/firmware-analysis/vela_ap_4.100.155.bin'

    def test_reviewed_target_has_independent_complete_body_proof(self):
        proof = next(row for row in compatibility.AUDIT['direct_dependency_proofs']
                     if row['address_155'] == '0xc9097b4')
        self.assertEqual(proof['address_139'], '0xc9097c4')
        self.assertEqual(proof['size'], 10)
        self.assertTrue(proof['exact_bytes'])
        self.assertEqual(proof['scope'], 'full IDA function range')
        self.assertEqual(native(0x0c9097b4, TARGETS[0]), 0x0c9097c4)

    def test_deleting_previously_unchecked_mapping_is_rejected(self):
        mapping = dict(compatibility.ADDRESS_MAP)
        del mapping[0x0c9097b4]
        with patch.dict(compatibility.ADDRESS_MAP, mapping, clear=True):
            with self.assertRaisesRegex(AssertionError, 'missing branch mapping'):
                validate_direct_branch(0x0c9097b4, 0x0c9097c4, 0, 0, 0)
            with self.assertRaisesRegex(AssertionError, 'lacks address mapping'):
                verify(self.ap139, self.ap155)

    def test_wrong_direct_mapping_is_rejected_even_at_allowed_boundary(self):
        with patch.dict(compatibility.ADDRESS_MAP, {0x0c9097b4: 0x0c9097c6}):
            with self.assertRaisesRegex(AssertionError, 'branch mapping differs'):
                validate_direct_branch(0x0c9097b4, 0x0c9097c4, 0, 0, 0,
                                       allow_boundary=True)
            with self.assertRaisesRegex(AssertionError, 'contradicts'):
                verify(self.ap139, self.ap155)

    def test_deleting_target_proof_cannot_leave_a_trusted_mapping(self):
        proofs = [row for row in compatibility.AUDIT['direct_dependency_proofs']
                  if row['address_155'] != '0xc9097b4']
        with patch.dict(compatibility.AUDIT, direct_dependency_proofs=proofs):
            with self.assertRaisesRegex(AssertionError, 'lacks code evidence'):
                verify(self.ap139, self.ap155)

    def test_injected_mapping_without_target_code_proof_is_rejected(self):
        with patch.dict(compatibility.ADDRESS_MAP, {0x0c123456: 0x0c123466}):
            with self.assertRaisesRegex(AssertionError, 'lacks code evidence'):
                validate_direct_branch(0x0c123456, 0x0c123466, 0, 0, 0)

    def test_ambiguous_anchor_is_not_independent_location_evidence(self):
        proof = compatibility.AUDIT['direct_dependency_proofs'][0]
        anchor = dict(proof['independent_anchor'], bytes='00' * 8)
        with patch.dict(proof, independent_anchor=anchor):
            with self.assertRaisesRegex(AssertionError, 'anchor missing or ambiguous'):
                verify(self.ap139, self.ap155)

    def test_jointly_tampered_mapping_and_proof_still_fail_anchor_location(self):
        proof = compatibility.AUDIT['direct_dependency_proofs'][0]
        old = int(proof['address_155'], 16)
        wrong = int(proof['address_139'], 16) + 2
        with patch.dict(proof, address_139=hex(wrong)), \
                patch.dict(compatibility.ADDRESS_MAP, {old: wrong}):
            with self.assertRaisesRegex(AssertionError, 'anchor does not locate mapped target'):
                verify(self.ap139, self.ap155)

    def test_unverified_boundary_cannot_silently_disappear_from_receipt(self):
        row = next(r for r in compatibility.AUDIT['functions']
                   if r['unverified_transitive_dependencies'])
        with patch.dict(row, unverified_transitive_dependencies=[]):
            with self.assertRaisesRegex(AssertionError, 'boundary receipt differs'):
                verify(self.ap139, self.ap155)

    def test_unverified_boundary_cannot_be_mislabeled_as_proven(self):
        row = next(r for r in compatibility.AUDIT['direct_dependency_proofs']
                   if r['unverified_transitive_dependencies'])
        dependencies = copy.deepcopy(row['unverified_transitive_dependencies'])
        dependencies[0]['status'] = 'VERIFIED'
        with patch.dict(row, unverified_transitive_dependencies=dependencies):
            with self.assertRaisesRegex(AssertionError, 'boundary receipt differs'):
                verify(self.ap139, self.ap155)

    def test_opted_in_unverified_boundary_is_returned_as_unverified(self):
        self.assertFalse(validate_direct_branch(0x0c123456, 0x0c123466, 0, 0, 0,
                                                allow_boundary=True))
        with self.assertRaisesRegex(AssertionError, 'missing branch mapping'):
            validate_direct_branch(0x0c123456, 0x0c123466, 0, 0, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
