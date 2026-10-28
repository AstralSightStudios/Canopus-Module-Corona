"""Run the production font transaction against the independently recovered P65 layout.

The shared fault/ownership fixture supplies modeled native leaves. Only fixture
memory geometry changes here; src/font_reload.c is included unchanged. Real
native leaves are exercised separately by firmware_p65_font.py.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


def fixture():
    s = (ROOT / 'tests/test_font_reload.c').read_text()
    s = s.replace('#include "../src/font_reload.c"', '#include "' + str(ROOT / 'src/font_reload.c') + '"')
    s = s.replace('globals[0x200000]', 'globals[0x600000]')
    s = s.replace('p>=0x20000000 && p-0x20000000', 'p>=0x3c000000 && p-0x3c000000').replace('globals+(p-0x20000000)', 'globals+(p-0x3c000000)')
    s = s.replace('ctx=heap(28)', 'ctx=heap(32)')
    s = s.replace('wr(ctx+20,256);', '')
    s = re.sub(r'ctx\+(4|8|12|16|24)\b', lambda m: 'ctx+' + {4:'12',8:'16',12:'20',16:'24',24:'28'}[int(m[1])], s)
    s = s.replace('for(q=rd(p+8);q;q=rd(q+12))', 'for(q=rd(p+16);q;q=rd(q+12))').replace('unlink_node(p+4,q)', 'unlink_node(p+12,q)')
    s = s.replace('rd(c+52)', 'rd(c+48)').replace('append(c+48,ll)', 'append(c+44,ll)').replace('unlink_node(c+48,ll)', 'unlink_node(c+44,ll)')
    s = s.replace('wr(p+40,rd(p+16)); wr(p+44,rd(p+4)+20); wr(p+48,4);', 'wr(p+36,rd(p+16)); wr(p+40,rd(p+4)+20); wr(p+44,4);')
    s = s.replace('expect_diagnostic(cache+48,8', 'expect_diagnostic(cache+44,8')
    s = s.replace('wr(RH_FW_FR_DISPLAY_LIST,792); display=heap(800)', 'wr(RH_FW_FR_DISPLAY_LIST,784); display=heap(792)')
    s = s.replace('wr(vg+16,', 'wr(vg+12,').replace('wr(sw+16,', 'wr(sw+12,').replace('wr(vg+32,', 'wr(vg+24,')
    s = s.replace('init_queue(glyph_queue,24,', 'init_queue(glyph_queue,72,').replace('wr(vg+48,glyph_queue)', 'wr(vg+28,glyph_queue)')
    s = s.replace('init_queue(imgq,76,', 'init_queue(imgq,4,').replace('wr(gradient+8,gradq); wr(vg+40,gradient)', 'wr(gradient+8,gradq); wr(vg+44,gradq)')
    s = s.replace('wr(obj,VECTOR_CLASS)', 'wr(obj,0x12345670u)').replace('vector_drops==1', 'vector_drops==0')
    start = s.index('    case (RH_FW_VECTOR_DROP & ~1u):')
    end = s.index('    case (RH_FW_OBJECT_STYLE_REFRESH & ~1u):', start)
    s = s[:start] + s[end:]
    start = s.index('    setup(); vec=heap(64);')
    end = s.index('    setup(); wr(glyph_queue+12,4097)', start)
    s = s[:start] + s[end:]
    s = s.replace('changed,child,vec,data,path', 'changed,child,data,path')
    s = s.replace('    case (RH_FW_FONT_SET_PIXEL_SIZE & ~1u):', '    case (RH_FW_FONT_SET_PIXEL_SIZE & ~1u):\n        assert(q==0); q=(uint32_t)c;')
    return s


class P65Font(unittest.TestCase):
    def test_switch_restore_rollback_and_ownership_faults(self):
        with tempfile.TemporaryDirectory(prefix='rh-p65-font-') as temporary:
            p = Path(temporary)
            source = p / 'fixture.c'; source.write_text(fixture())
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g',
                '-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-DRH_TARGET_P65=1','-I'+str(ROOT/'include'),
                str(source),str(ROOT/'src/resource_hook.c'),'-o',str(p/'fixture')],check=True)
            subprocess.run([str(p/'fixture')],check=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
