"""Compile the production P65 blur provider with modeled native leaves."""
import os
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class P65Blur(unittest.TestCase):
    def test_production_provider_lifecycle_and_pixels(self):
        with tempfile.TemporaryDirectory(prefix='p65-blur-') as directory:
            executable=Path(directory)/'blur'
            subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O1','-g',
                '-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DRH_TARGET_P65=1',
                '-I'+str(ROOT/'include'),str(ROOT/'tests/p65_blur_harness.c'),
                '-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True)

    def test_exact_body_hashes_and_native_drawbuffer_contract(self):
        from p65_firmware import firmware
        try:
            data=firmware()
        except FileNotFoundError as error:
            self.skipTest(str(error))
        evidence=json.loads((ROOT/'targets/redmi-watch-6-3.100.043/blur-evidence.json').read_text())
        for function in evidence['functions']:
            offset=int(function['file_offset'],0)
            body=data[offset:offset+function['size']]
            self.assertEqual(hashlib.sha256(body).hexdigest(),function['sha256'],function['name'])
            self.assertEqual(body[:len(bytes.fromhex(function['witness']))].hex(),function['witness'])
        try:
            from firmware_p65 import Machine
        except ModuleNotFoundError as error:
            self.skipTest('Native instruction test dependencies unavailable: '+str(error))
        core=json.loads((ROOT/'targets/redmi-watch-6-3.100.043/core-evidence.json').read_text())
        machine=Machine(data,core)
        descriptor=0x02020000
        allocation=0x3c500013
        calls=[]
        machine.hooks[0x0c741b20]=lambda *args: descriptor
        def allocate(size,cf,*unused):
            calls.append(('alloc',size,cf));return allocation
        def free_pixels(pointer,*unused):
            calls.append(('pixels_free',pointer))
        def free_descriptor(pointer,*unused):
            calls.append(('descriptor_free',pointer))
        machine.hooks[0x02000100]=allocate
        machine.hooks[0x02000104]=free_pixels
        machine.hooks[0x0c741b48]=free_descriptor
        machine.hooks[0x0c8bf0b0]=lambda *args:1
        machine.put(0x3c450108,0x02000101,0x02000105,0x0c719111,0x0c72f021,0x0c72f02d,0x0c728755)
        self.assertEqual(machine.call(0x0c72e0a5,15),0x428)
        self.assertEqual(machine.call(0x0c72e0a5,18),0x405)
        self.assertEqual(machine.call(0x0c728755,432,15),1296)
        self.assertEqual(machine.call(0x0c728755,72,18),160)
        self.assertEqual(machine.call(0x0c719551,432,514,15,0),descriptor)
        self.assertEqual(machine.get(descriptor),0x120f19)
        self.assertEqual(machine.get(descriptor+4),432|(514<<16))
        self.assertEqual(machine.get(descriptor+8)&65535,1296)
        self.assertEqual(machine.get(descriptor+12),1296*514)
        self.assertEqual(machine.get(descriptor+16),0x3c500040)
        self.assertEqual(machine.get(descriptor+20),allocation)
        self.assertEqual(calls,[('alloc',1296*514,15)])
        machine.call(0x0c72f021,allocation,1296*514,15,0)
        machine.call(0x0c72f02d,descriptor,0)
        machine.call(0x0c719799,descriptor)
        self.assertEqual(calls,[('alloc',1296*514,15),('pixels_free',allocation),('descriptor_free',descriptor)])

if __name__=='__main__': unittest.main(verbosity=2)
