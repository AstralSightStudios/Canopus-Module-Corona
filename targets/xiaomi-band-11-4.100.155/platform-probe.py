"""Bounded .155 platform probe; modeled leaves are explicit below.

Build the fixed-address platform binary as documented in ui-reload-audit.md.
Requires Unicorn. This is not a signed-loader or physical-device acceptance test.
"""
from pathlib import Path
import argparse
import struct, hashlib
from unicorn import *
from unicorn.arm_const import *
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--firmware',type=Path,required=True)
parser.add_argument('--binary',type=Path,required=True)
parser.add_argument('--symbols',type=Path,required=True)
args=parser.parse_args()
fw=args.firmware.read_bytes()
assert hashlib.sha256(fw).hexdigest()=='ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f'
u=Uc(UC_ARCH_ARM,UC_MODE_THUMB|UC_MODE_MCLASS)
u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M33)
for a,n in [(0xc0c0000,(len(fw)+4095)&~4095),(0x2c0c0000,(len(fw)+4095)&~4095),(0x20000000,0x160000),(0x3c700000,0x100000),(0x3c350000,0x20000),(0x1c700000,0x20000)]:u.mem_map(a,n)
u.mem_write(0xc0c0000,fw);u.mem_write(0x2c0c0000,fw)
u.mem_write(0x1c700000,args.binary.read_bytes())
symbols={s[2]:int(s[0],16) for l in args.symbols.read_text().splitlines() if len(s:=l.split())==3}
regs=[UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3]
def reg(i):return u.reg_read(regs[i])
def word(a,v=None):
 if v is None:return struct.unpack('<I',u.mem_read(a,4))[0]
 u.mem_write(a,struct.pack('<I',v))
def byte(a,v):u.mem_write(a,bytes([v]))
def hook(a,fn):
 def cb(u,a,n,data):
  v=fn()
  if v is not None:u.reg_write(UC_ARM_REG_R0,v&0xffffffff)
  u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
 return u.hook_add(UC_HOOK_CODE,cb,None,a,a)
def call(name,*args):
 for r in regs:u.reg_write(r,0)
 for r,v in zip(regs,args):u.reg_write(r,v&0xffffffff)
 u.reg_write(UC_ARM_REG_SP,0x20150000);u.reg_write(UC_ARM_REG_LR,0x1c71ff01)
 u.emu_start((symbols[name] if isinstance(name,str) else name)|1,0x1c71ff00,count=100000)
 assert u.reg_read(UC_ARM_REG_PC)==0x1c71ff00,hex(u.reg_read(UC_ARM_REG_PC))
 return reg(0)
# Driver and native I/O argument/entry selection.
word(0x200bd3b8,47);word(0x200bd3bc,4096)
assert call('rh_platform_driver_valid')==1
assert call('rh_platform_driver')==0x200bd3b8
assert call('rh_platform_slot')==0x200bd3c4
assert call('rh_platform_original')==0xc3a6195
for name,a,args in [('open',0xc342c54,(0x3c700000,1)),('read',0xc33d784,(3,0x3c700000,10)),('write',0xc33dc4e,(3,0x3c700000,10)),('close',0xc33818c,(3,))]:
 seen=[]
 h=hook(a,lambda:seen.append(tuple(reg(i) for i in range(len(args)))) or 7)
 call('rh_platform_'+name,*args);assert seen==[args];u.hook_del(h)
print('PASS driver and native I/O dispatch')
# Emitted SDK temp allocator: verified firmware entries, modeled heap functions.
heap,allocation=0x3c356b40,0x3c700080
word(0x200b2590,heap);word(heap+28,heap+0x168);word(heap+32,0x3cfefdf8)
seen=[];largest=296 # rounded 32-byte request = 40, plus margin 256

def mallinfo():
 assert reg(1)==heap
 u.mem_write(reg(0),struct.pack('<7I',0,0,0,largest,0,0,0))
 seen.append('mallinfo');return reg(0)
def memalign():
 assert (reg(0),reg(1),reg(2))==(heap,8,32)
 seen.append('memalign');return allocation
def free():
 assert reg(0)==allocation
 seen.append('free');return 0
hs=[hook(0xc34f0a0,mallinfo),hook(0xc3507e8,memalign),hook(0xc34cd2c,free)]
assert call('rh_platform_alloc',32)==allocation
call('rh_platform_free',allocation)
assert seen==['mallinfo','memalign','free']
largest=295
assert call('rh_platform_alloc',32)==0 and seen[-1]=='mallinfo'
assert seen.count('memalign')==1
word(0x200b2590,heap+8)
assert call('rh_platform_alloc',32)==0 and seen.count('mallinfo')==2
word(0x200b2590,heap)
call('rh_platform_free',0);assert seen.count('free')==1
for h in hs:u.hook_del(h)
print('PASS generated SDK allocator ABI/entries, heap identity and headroom gates')
# Mapping-filtered cache/owner adoption now has dedicated source-compiled probes
# in tests/firmware_reload.py; no global retirement entry point remains.
# Readiness and native rotation-aware accessors, modeled invalidation only.
disp=0x3c706000;word(0x200bd200,disp);word(disp+696,0x3c707000);word(disp+608,1)
word(disp,212);word(disp+4,520)
assert call('rh_platform_redraw_ready')==1
byte(disp+58,2);assert call('rh_platform_redraw_ready')==0;byte(disp+58,0)
def inv():
 assert reg(0)==disp
 assert bytes(u.mem_read(reg(1),16))==struct.pack('<4i',0,0,0x7fff,0x7fff)
 width,height=(520,212) if u.mem_read(disp+756,1)==b'\x02' else (212,520)
 word(disp+604,1);u.mem_write(disp+60,struct.pack('<4i',0,0,width-1,height-1));return 0
h=hook(0xc382428,inv)
for rotation in [0,2]:
 byte(disp+756,rotation);assert call('rh_platform_request_full_redraw')==0
u.hook_del(h)
print('PASS redraw readiness and native resolution accessors at both rotations')
# Forced page rebuilding was removed from the module. Native lifecycle
# counterexamples remain in tests/firmware_page_rebuild.py, not activation.
# Font registry node offsets and the target-specific remove entry.
uikit,manager,fnode,fname,fpath=0x3c720000,0x3c721000,0x3c722000,0x3c723000,0x3c724000
word(0x200bd1e8,uikit);word(uikit+28,manager);word(manager+24,8);word(manager+28,fnode)
word(fnode,fname);word(fnode+4,fpath)
u.mem_write(fname,b'MiSans\0');u.mem_write(fpath,b'/old.ttf\0')
assert call('rh_platform_font_path_get',0,0x3c725000,0x3c726000)==0
assert bytes(u.mem_read(0x3c725000,7))==b'MiSans\0'
assert bytes(u.mem_read(0x3c726000,9))==b'/old.ttf\0'
assert call('rh_platform_font_path_get',1,0x3c725000,0x3c726000)==0xffffffff
u.mem_write(0x3c727000,b'/new.ttf\0');seen=[]
def remove():assert reg(0)==fnode;seen.append('remove');return 0
def add():assert bytes(u.mem_read(reg(0),7))==b'MiSans\0';assert reg(1)==0x3c727000;seen.append('add');return 0
def resolve():assert reg(0)==manager;seen.append('resolve');return 0x3c727000
hs=[hook(0xc904cec,remove),hook(0xc4924e0,add),hook(0xc490edc,resolve)]
assert call('rh_platform_font_retarget',0,0x3c727000)==0;assert seen==['remove','add','resolve']
for h in hs:u.hook_del(h)
print('PASS font registry layout and remove/add/resolve dispatch')
seen=[]
def timer_create():assert (reg(0),reg(1),reg(2))==(0x1c71f001,50,0);seen.append('create');return 0x3c730000
def timer_delete():assert reg(0)==0x3c730000;seen.append('delete');return 0
hs=[hook(0xc3abd20,timer_create),hook(0xc3abe70,timer_delete)]
assert call('rh_platform_refresh_timer_create',0x1c71f001)==0x3c730000
call('rh_platform_refresh_timer_delete',0x3c730000);assert seen==['create','delete']
print('PASS timer dispatch')
