"""Exact P65 font factory/cache and descriptor instruction probes.

Original P65 instructions execute; FreeType, allocation and selected libc/cache
leaves are modeled. Production transaction fault tests are test_p65_font.py.
"""
import hashlib
import json
from pathlib import Path
import unittest
from firmware_p65 import Machine, SCRATCH
from p65_firmware import firmware, ROOT


class P65NativeFont(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data=firmware()
        cls.core=json.loads((ROOT/'targets/redmi-watch-6-3.100.043/core-evidence.json').read_text())
        cls.audit=json.loads((ROOT/'targets/redmi-watch-6-3.100.043/font-reload-audit.json').read_text())

    def machine(self):
        return Machine(self.data,self.core)

    def test_all_font_identities_match_exact_ap_witnesses(self):
        for item in self.audit['functions']:
            with self.subTest(name=item['name']):
                offset=int(item['file_offset'],0)
                expected=bytes.fromhex(item['bytes'])
                self.assertEqual(self.data[offset:offset+len(expected)],expected)
                self.assertEqual(hashlib.sha256(expected).hexdigest(),item['sha256'])

    def test_native_factory_builds_the_expected_descriptor_and_callbacks(self):
        m=self.machine()
        ctx,cache,node,path,face,ft,metrics = [SCRATCH+i*0x100 for i in range(7)]
        entry=face+24
        m.put(0x3c450204,ctx)
        m.put(ctx,ft,0,metrics,8,node,node,0x0c729aa5,cache)
        m.put(node,path,1,0,0)
        m.uc.mem_write(path,b'/system/fonts/Regular.ttf\0')
        m.put(face,path,65536,128,ft,SCRATCH+0x1100,SCRATCH+0x1200)
        m.put(entry,cache,1,24,0)
        m.put(ft+88,metrics)
        m.put(metrics+20,65536)
        m.put(metrics+28,0xffffff80,1536)
        m.uc.mem_write(ft+80,b'\x02\x00\x01\x00')
        allocations=[]; pixel_calls=[]
        def strlen(address,*unused):return len(m.string(address))
        def strcmp(a,b,*unused):return 0 if m.string(a)==m.string(b) else 1
        def zero_alloc(size,*unused):
            address=SCRATCH+0x2000+len(allocations)*0x100
            allocations.append((address,size));m.uc.mem_write(address,bytes(size));return address
        def pixel(face_address,width,height,*unused):
            pixel_calls.append((face_address,width,height));return 0
        m.hooks[0xc741aa4]=strlen
        m.hooks[0xc8bd1d8]=strcmp
        m.hooks[0xc73b964]=lambda *unused:entry
        m.hooks[0xc741b20]=zero_alloc
        m.hooks[0xc5666fc]=pixel
        # run(path,render_mode=1,pixels=24,style=0) follows the existing-face path.
        result=m.call(0xc7344fc,path,1,24,0)
        self.assertEqual(len(allocations),1)
        d=allocations[0][0]
        self.assertEqual(result,d+4)
        self.assertEqual([m.get(d+i) for i in (0,4,8,12,28,40,48,52,56,60)],
                         [0x5f5f4654,0xc734999,0xc734e49,0xc734e2d,d,24,ctx,face,entry,path])
        self.assertEqual(pixel_calls,[(ft,0,24)])
        self.assertEqual(m.get(node+4),2)
        self.assertEqual(m.get(d+16),24)
        self.assertEqual(m.get(d+20),2)

    def test_native_count_cache_init_produces_p65_link_offsets(self):
        m=self.machine();cache=SCRATCH
        m.put(cache,0x3c20c7c8,32,512,0,0xc734a99,0xc734899,0xc73488d)
        calls=[]
        def tree(head,compare,node_size,*unused):
            calls.append((head,compare,node_size));return 1
        m.hooks[0xc73f87c]=tree
        self.assertEqual(m.call(0xc73b5e8,cache),1)
        self.assertEqual(m.get(cache+44),4)
        self.assertEqual(m.get(cache+48),0)
        self.assertEqual(m.get(cache+52),0)
        self.assertEqual(calls[0][:2],(cache+32,0xc734a99))


if __name__=='__main__':unittest.main(verbosity=2)
