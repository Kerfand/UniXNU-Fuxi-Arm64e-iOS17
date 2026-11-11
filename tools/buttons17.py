"""buttons17.py (2026-11-11): wire the fuxi buttons (kits/sheng/FuxiButtons.cpp: PMK8550 PON/RESIN + PM8550 GPIO6,
SPMI observer polling) into the iOS 17 build, per docs/zh/porting/input.md section 4 (report 2 Power / Volume Increment /
Volume Decrement). The iOS 17 HID device runs digitizer-only (fuxi-touch-digitizer-only=1), which had no report 2 and never
registered the button sink; with fuxi-buttons=1 a Consumer Control application collection (report 2, same bit layout as the
shared descriptor) is appended after the digitizer collection, so the digitizer stays the first/primary usage."""
from pathlib import Path
R = Path('/home/xiaomi/sadnut-fuxi-local-20261103')
S = R / 'device-ios17/scripts'


def edit(path, old, new, marker):
    t = path.read_text()
    if marker in t:
        print('already', path.name, marker[:40]); return
    assert t.count(old) == 1, (path.name, old[:60])
    path.write_text(t.replace(old, new)); print('edited', path.name)


# 1. HID transform
h = S / 'build-ios17-hid-input.py'
edit(h,
     "SRC.write_text('// ios17 copy (build-ios17-hid-input.py): the original-ABI header is generated for 21H16 arm64e.\\n'\n",
     "# fuxi buttons (docs/zh/porting/input.md section 4): with fuxi-buttons=1 the digitizer-only device also carries a Consumer\n"
     "# Control collection (report 2: bit0 Menu, bit1 Power, bit2 Volume Increment, bit3 Volume Decrement, as the shared descriptor)\n"
     "# after the digitizer, so the digitizer stays the primary usage, and registers the button sink (FuxiButtons.cpp).\n"
     "shared = shared.replace(start_marker + ' unsigned digitizerOnly=0; ', start_marker + ' unsigned digitizerOnly=0; '\n"
     "    '{unsigned b=0; ios17TouchButtons=PE_parse_boot_argn(\"fuxi-buttons\",&b,sizeof(b)) && b==1;} ')\n"
     "desc_old = 'if(touchOnly){size=sizeof(touchDescriptor);memcpy(combined,touchDescriptor,size);}'\n"
     "require(shared.count(desc_old) == 1, 'touch-only descriptor anchor')\n"
     "shared = shared.replace(desc_old, 'if(touchOnly){size=sizeof(touchDescriptor);memcpy(combined,touchDescriptor,size);'\n"
     "    'if(ios17TouchButtons){memcpy(combined+size,ios17ConsumerDescriptor,sizeof(ios17ConsumerDescriptor));size+=sizeof(ios17ConsumerDescriptor);}}')\n"
     "for sink_old in ('if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(&NativeHidInput::buttonsNotify,this);',\n"
     "                 'if(!touchOnly && &native_buttons_set_sink)native_buttons_set_sink(nullptr,nullptr);'):\n"
     "    require(shared.count(sink_old) == 1, 'button sink anchor')\n"
     "    shared = shared.replace(sink_old, sink_old.replace('!touchOnly', '(!touchOnly || ios17TouchButtons)'))\n"
     "desc_anchor = '#define TOUCH_FINGERS 10\\n'\n"
     "require(shared.count(desc_anchor) == 1, 'TOUCH_FINGERS anchor')\n"
     "shared = shared.replace(desc_anchor, desc_anchor + 'static bool ios17TouchButtons;\\n'\n"
     "    'static_assert(sizeof(descriptor) >= 33, \"consumer collection fits the combined buffer\");\\n'\n"
     "    'static const unsigned char ios17ConsumerDescriptor[]={0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x02,'\n"
     "    '0x09,0x40,0x09,0x30,0x09,0xe9,0x09,0xea,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x04,0x81,0x02,'\n"
     "    '0x75,0x04,0x95,0x01,0x81,0x03,0xc0};\\n')\n"
     "SRC.write_text('// ios17 copy (build-ios17-hid-input.py): the original-ABI header is generated for 21H16 arm64e.\\n'\n",
     'ios17TouchButtons')
edit(h,
     "stub.write_text('// build-ios17-hid-input.py: no board buttons in the iOS 17 port yet.\\n#include <stdint.h>\\n'\n"
     "                'void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context);\\n'\n"
     "                'void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {(void)sink; (void)context;}\\n')\n",
     "stub.write_text('// build-ios17-hid-input.py: weak fallback; FuxiButtons.cpp (display archive) defines the real sink.\\n#include <stdint.h>\\n'\n"
     "                'void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context);\\n'\n"
     "                '__attribute__((weak)) void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {(void)sink; (void)context;}\\n')\n",
     'weak fallback; FuxiButtons')
# 2. compile FuxiButtons.cpp into the display archive
l = S / 'build-ios17-legacy-display.py'
edit(l,
     "for src in (touch_src, ROOT / 'kits/sheng/FuxiHidProbe.cpp', ROOT / 'kits/sheng/HidInputProbe.cpp'):\n",
     "for src in (touch_src, ROOT / 'kits/sheng/FuxiHidProbe.cpp', ROOT / 'kits/sheng/HidInputProbe.cpp',\n"
     "            ROOT / 'kits/sheng/FuxiButtons.cpp'):   # fuxi buttons (fuxi-buttons=1), docs/zh/porting/input.md section 4\n",
     'kits/sheng/FuxiButtons.cpp')
# 3. weak stats stub
st = S / 'build-ios17-stubs.py'
edit(st, "volatile int32_t fuxi_buttons_stats[10];\n", "volatile int32_t fuxi_buttons_stats[10] __attribute__((weak));\n",
     'fuxi_buttons_stats[10] __attribute__((weak))')
# 4. late probe call
p = S / 'data/Ios17LegacyDisplayProbe.cpp'
edit(p, "extern \"C\" void native_touch_probe(void);\n",
     "extern \"C\" void native_touch_probe(void);\n"
     "// The fuxi buttons (kits/sheng/FuxiButtons.cpp, boot-arg fuxi-buttons=1): PMK8550 PON/RESIN and PM8550 GPIO6 over the SPMI\n"
     "// observer, reported through NativeHidInput report 2.\n"
     "extern \"C\" void fuxi_buttons_probe(void);\n", 'fuxi_buttons_probe(void);')
edit(p, "\tnative_touch_probe();\n", "\tnative_touch_probe();\n\tfuxi_buttons_probe();\n", '\tfuxi_buttons_probe();')
# 5. boot-arg
pk = R / 'pack-ios17-hidpath.py'
edit(pk, "        + 'fuxi-touch-autotap=60 '\n",
     "        + 'fuxi-touch-autotap=60 '\n"
     "        # Buttons (kits/sheng/FuxiButtons.cpp): power / volume through NativeHidInput report 2.\n"
     "        + 'fuxi-buttons=1 '\n", 'fuxi-buttons=1')
print('done')
