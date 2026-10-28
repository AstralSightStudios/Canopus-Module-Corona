"""Compiled P65 calendar adapter + exact native premultiply instruction tests.

OS/LVGL/GPU leaves are controlled; no rendering or physical GPU claim.
"""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest
from firmware_p65 import Machine
from p65_firmware import ROOT, firmware


class P65Calendar(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ap = firmware()
        cls.evidence = json.loads((ROOT/'targets/redmi-watch-6-3.100.043/core-evidence.json').read_text())
        cls.addresses = {k: int(v, 0) for k, v in re.findall(r'#define RH_P65_CN_(\w+) (0x[0-9a-f]+|[0-9]+)u',
            (ROOT/'include/resource_hook_p65_calendar.h').read_text())}
        cls.tmp = tempfile.TemporaryDirectory(prefix='p65-calendar-')
        cls.addClassCleanup(cls.tmp.cleanup)
        directory = Path(cls.tmp.name)
        shim = directory/'shim.c'
        shim.write_text('''#include <stdint.h>
int rh_platform_open(const char *p,int m){(void)p;(void)m;return -1;}
int rh_platform_write(int fd,const void *p,uint32_t n){(void)fd;(void)p;(void)n;return -1;}
void rh_platform_close(int fd){(void)fd;}
''')
        elf=directory/'calendar.elf'
        subprocess.run([os.environ.get('CLANG','clang'),'--target=arm-none-eabi','-mcpu=cortex-m33',
            '-mthumb','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector',
            '-nostdlib','-O2','-Wall','-Wextra','-Werror','-DRH_TARGET_P65=1','-DRH_NATIVE_MANAGER=1',
            '-I'+str(ROOT/'include'),'-fuse-ld=lld','-Wl,-Ttext=0x01000000',
            '-Wl,-e,rh_calendar_native_render',str(ROOT/'src/calendar_native.c'),
            str(ROOT/'src/calendar_config.c'),str(shim),'-o',str(elf)],check=True)
        cls.elf=elf.read_bytes()

    def setUp(self):
        self.m=Machine(self.ap,self.evidence,self.elf)
        self.next=0x3c500000
        self.trace=[];self.fences=[];self.fonts=[];self.closed=[];self.written=bytearray()
        self.draw_ok=True;self.create_ok=True;self.image_ok=True;self.write_limit=4096;self.write_fail=False
        self.when=(2026,10,28,2);self.clock_ok=True;self.lookup_ok=True;self.stock_exists=True
        self.output=self.text('/data/canopus/m.corona/calendar-alpha-0.bin')
        self.config=self.mem(13092);self.date=self.mem(20)
        self.call('rh_calendar_defaults',self.config)
        self.word(self.config+13088,1)
        self.m.uc.mem_write(self.date,struct.pack('<5I',*self.when,0))
        root=self.mem(64);manager=self.mem(600)
        self.word(self.addresses['UIKIT_SLOT'],root);self.word(root+28,manager)
        self.manager=manager
        self.word(self.addresses['HANDLER_TABLE']+12,self.addresses['CACHE_INVALIDATE']|1)
        self.word(self.addresses['HANDLER_TABLE']+16,self.addresses['CACHE_CLEAN']|1)
        unit=self.mem(64);self.word(self.addresses['DRAW_UNITS'],unit)
        self.word(unit+12,self.addresses['VG_DISPATCH'])
        self.locale=self.text('Wednesday')
        for name in ('CLOCK','LOCALTIME','LOCALIZED','ROOT_CREATE','SIZE','IMAGE_CREATE','IMAGE_SOURCE',
                     'IMAGE_INFO','LABEL_CREATE','LABEL_TEXT','COLOR','TEXT_ALIGN','FONT_STYLE','FONT_RESOLVE',
                     'FONT_CREATE','FONT_RELEASE','ALIGN','DRAW_BUF_CREATE','DRAW_BUF_DESTROY','SNAPSHOT',
                     'OBJECT_DELETE','GPU_FINISH','NATIVE_WAIT','CACHE_CLEAN','BUF_INVALIDATE','LAUNCHER_UPDATE'):
            self.m.hooks[self.addresses[name]&~1]=lambda *args,n=name:self.leaf(n,args)
        for name in ('rh_platform_open','rh_platform_write','rh_platform_close'):
            self.m.hooks[self.m.symbols[name]&~1]=lambda *args,n=name:self.platform(n,args)

    def mem(self,n=128):
        a=self.next;self.next+=(n+15)&~15;self.m.uc.mem_write(a,bytes(n));return a
    def text(self,s):
        d=s.encode()+b'\0';a=self.mem(len(d));self.m.uc.mem_write(a,d);return a
    def word(self,a,v=None):
        if v is None:return self.m.get(a)
        self.m.put(a,v)
    def string(self,a):return self.m.string(a).decode()
    def call(self,name,*args):return self.m.call(self.m.symbols[name],*args)
    def render(self):return self.call('rh_calendar_native_render',self.config,self.date,self.output)

    def leaf(self,name,args):
        a,b,c,d=args;self.trace.append((name,args))
        if name=='CLOCK':
            if not self.clock_ok:return -1
            self.m.put(b,123,0,0);return 0
        if name=='LOCALTIME':
            y,mo,day,wd=self.when
            self.m.uc.mem_write(c,struct.pack('<11I',0,0,0,day,mo-1,y-1900,(wd+1)%7,0,0,0,0));return c
        if name=='LOCALIZED':return self.locale
        if name=='GPU_FINISH':return self.fences.pop(0) if self.fences else 0
        if name in ('ROOT_CREATE','LABEL_CREATE','IMAGE_CREATE'):
            return self.mem() if self.create_ok else 0
        if name=='IMAGE_INFO':
            self.m.uc.mem_write(b,struct.pack('<BB5H',25,16,0,24,32,96,0));return 1 if self.image_ok else 0
        if name=='IMAGE_SOURCE':
            if self.image_ok:self.m.put(a+52,b);self.m.put(a+64,24,32)
            return 0
        if name=='FONT_RESOLVE':return self.mem(40) if self.lookup_ok else 0
        if name=='FONT_CREATE':
            self.assertEqual(a,self.manager);self.assertEqual(self.string(self.word(b)),'MiSans-Medium')
            font=self.mem(40);self.fonts.append(font);return font if self.lookup_ok else 0
        if name=='FONT_RELEASE':
            self.assertEqual(a,self.manager);self.assertIn(b,self.fonts);return 1
        if name=='DRAW_BUF_CREATE':
            self.assertEqual((a,b,c,d),(96,96,16,0))
            self.buffer=self.mem(24);self.pixels=self.mem(96*96*4)
            self.m.uc.mem_write(self.buffer,struct.pack('<BB5H3I',25,16,18,96,96,384,0,96*96*4,self.pixels,self.pixels))
            return self.buffer
        if name=='SNAPSHOT':
            self.assertEqual((b,c),(16,self.buffer))
            self.m.uc.mem_write(self.pixels,bytes([100,50,20,128])*(96*96));return int(self.draw_ok)
        if name=='LAUNCHER_UPDATE':self.assertEqual(a,69)
        return 0

    def platform(self,name,args):
        a,b,c,d=args;self.trace.append((name,args))
        if name=='rh_platform_open':
            if b==1:
                self.assertEqual(self.string(a),'/data/quickapp/app/perpetual_calendar/calendar_icon.bin')
                return 8 if self.stock_exists else -2
            self.assertEqual((self.string(a),b),('/data/canopus/m.corona/calendar-alpha-0.bin',2));return 7
        if name=='rh_platform_close':self.closed.append(a);return 0
        self.assertEqual(a,7)
        if self.write_fail:return -1
        n=min(c,self.write_limit);self.written.extend(self.m.uc.mem_read(b,n));return n

    def test_date_locale_and_clock_failure(self):
        self.assertEqual(self.call('rh_calendar_native_date',self.date),0)
        self.assertEqual(struct.unpack('<4I',self.m.uc.mem_read(self.date,16)),self.when)
        before=bytes(self.m.uc.mem_read(self.date,20));self.clock_ok=False
        self.assertEqual(self.call('rh_calendar_native_date',self.date),-1)
        self.assertEqual(bytes(self.m.uc.mem_read(self.date,20)),before)

    def test_custom_text_size_natural_image_and_native_pixel_premultiply(self):
        # month custom label with independent managed wrapper; day uses native46.
        self.word(self.config,1);self.word(self.config+16,28)
        self.assertEqual(self.render(),0)
        self.assertEqual(len(self.fonts),1)
        self.assertEqual(self.written[:12],struct.pack('<BB5H',25,16,1,96,96,384,0))
        self.assertEqual(self.written[12:16],bytes([50,25,10,128]))
        self.assertEqual(len(self.written),12+96*96*4)
        names=[n for n,a in self.trace]
        self.assertLess(names.index('GPU_FINISH',names.index('SNAPSHOT')),names.index('BUF_INVALIDATE'))
        self.assertLess(names.index('CACHE_CLEAN'),names.index('SNAPSHOT'))
        self.assertLess(names.index('OBJECT_DELETE'),names.index('FONT_RELEASE'))
        self.assertLess(names.index('NATIVE_WAIT',names.index('SNAPSHOT')),names.index('OBJECT_DELETE'))
        self.assertEqual(self.closed,[8,7])

    def test_failed_fence_retains_tree_buffer_and_fonts_until_reset(self):
        self.word(self.config,1);self.word(self.config+16,28)
        self.fences=[0,12]
        self.assertEqual(self.render(),-1)
        self.assertNotIn('OBJECT_DELETE',[n for n,a in self.trace]);self.assertEqual(self.closed,[8])
        self.fences=[12]
        self.assertEqual(self.call('rh_p65_calendar_reset'),-1)
        self.assertNotIn('DRAW_BUF_DESTROY',[n for n,a in self.trace])
        self.fences=[0]
        self.assertEqual(self.call('rh_p65_calendar_reset'),0)
        self.assertEqual([n for n,a in self.trace].count('OBJECT_DELETE'),1)
        self.assertEqual([n for n,a in self.trace].count('FONT_RELEASE'),1)

    def test_failed_snapshot_and_short_write_do_not_report_success(self):
        self.draw_ok=False;self.assertEqual(self.render(),-1);self.assertFalse(self.written)
        self.closed.clear()
        self.draw_ok=True;self.write_fail=True
        self.assertEqual(self.render(),-1);self.assertEqual(self.closed,[8,7])

    def test_custom_images_keep_bin_dimensions_and_missing_info_refuses(self):
        # day image item: context index27 (day28) in fixed config pool.
        item=self.config+96
        self.word(item,2);self.word(item+28,31)
        pool=self.config+288+4
        # actual struct pool starts after items and pool_used; get layout at compile-time convention.
        relative=b'themes/demo/day.bin\0'
        self.m.uc.mem_write(pool,relative)
        self.word(self.config+288,len(relative))
        for i in range(31):self.m.uc.mem_write(item+32+2*i,struct.pack('<H',0))
        self.assertEqual(self.render(),0)
        self.assertTrue(any(n=='IMAGE_SOURCE' and self.string(a[1]).endswith('/themes/demo/day.bin') for n,a in self.trace))
        self.image_ok=False;self.assertEqual(self.render(),-1)

    def test_missing_stock_producer_refuses_instead_of_false_publication(self):
        self.stock_exists=False
        self.assertEqual(self.render(),-1)
        self.assertNotIn('SNAPSHOT',[n for n,a in self.trace])
        self.assertFalse(self.written)

    def test_wrong_receipt_refuses_before_native_calls(self):
        self.word(self.addresses['GENERATE'],0)
        self.assertEqual(self.render(),-1)
        self.assertEqual(self.call('rh_calendar_native_date',self.date),-1)
        self.assertEqual(self.trace,[])

    def test_active_draw_unit_blocks_cleanup_until_later_owner_turn(self):
        unit=self.word(self.addresses['DRAW_UNITS'])
        self.word(unit+24,0x3c500000)
        self.assertEqual(self.render(),-1)
        self.assertNotIn('ROOT_CREATE',[n for n,a in self.trace])
        self.word(unit+24,0)
        self.assertEqual(self.render(),0)

    def test_default_restore_refreshes_stock_without_rewriting_original(self):
        self.assertEqual(self.call('rh_p65_calendar_reset'),0)
        self.call('rh_p65_calendar_publish')
        self.assertEqual([n for n,a in self.trace],['LAUNCHER_UPDATE'])
        self.assertFalse(self.written)

if __name__=='__main__':unittest.main(verbosity=2)
