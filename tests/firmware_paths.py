"""Execute actual .139 LVGL file functions. VFS/allocator remain modeled."""
import sys
from pathlib import Path
import unittest
import os
ROOT = Path(__file__).resolve().parents[1]
CANOPUS = Path(os.environ.get('CANOPUS_ROOT', ROOT.parent / 'Canopus')).resolve()
sys.path.insert(0, str(CANOPUS / 'scripts/tests'))
from band11_arm_bootstrap import Machine
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R1, UC_ARM_REG_R2

class Paths(unittest.TestCase):
    def test_driver_nodes_contain_pointers_not_inline_drivers(self):
        m=Machine(); node=0x3c700000; driver=0x200bd3b8
        m.word(0x200bd3ac,4);m.word(0x200bd3b0,node)
        m.word(node,driver);m.word(node+8,0)
        m.word(driver,ord('/'))
        self.assertEqual(m.call(0x0c3a4360,ord('/')),driver)
        self.assertEqual(m.call(0x0c3a4360,ord('S')),0)
    def test_posix_open_adds_slash_and_encodes_descriptor(self):
        m=Machine(); path=0x3c710000
        seen=[]
        def opened():
            seen.append((m.string(m.reg(0)),m.reg(1)))
            return 0 # descriptor zero is valid; LVGL must return one
        m.uc.hook_del(m.firmware_hooks[0x0c342c54])
        m.uc.hook_add(UC_HOOK_CODE,m.firmware_call,opened,0x0c342c54,0x0c342c54)
        m.uc.mem_write(path,b'data/canopus/themes/a.bin\0')
        m.uc.reg_write(UC_ARM_REG_R1,path);m.uc.reg_write(UC_ARM_REG_R2,2)
        self.assertEqual(m.call(0x0c3a6194,0x200bd3b8),1)
        self.assertEqual(seen,[('/data/canopus/themes/a.bin',1)])
if __name__=='__main__':unittest.main()
