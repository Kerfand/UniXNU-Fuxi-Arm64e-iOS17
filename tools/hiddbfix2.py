"""hiddbfix2.py: IOHIDFamily-2238 MatchPropertyTable rejects a personality carrying HIDDefaultBehavior when
!owner->getProperty("HIDDefaultBehavior"); IOHIDInterface::matchPropertyTable delegates to provider (the IOHIDDevice)
unless the device is multi-interface. So the key belongs on NativeHidInput (the device), not on the interface."""
from pathlib import Path
h = Path('/home/xiaomi/sadnut-fuxi-local-20261103/device/package/HidInputProbe.cpp')
t = h.read_text()
old = ("        if(iface && noDriverRounds==5 && !iface->getProperty(\"HIDDefaultBehavior\")) {\n"
       "            iface->setProperty(\"HIDDefaultBehavior\",\"\");\n")
new = ("        if(iface && noDriverRounds==5 && !device->getProperty(\"HIDDefaultBehavior\")) {\n"
       "            // Presence-only key, checked on the IOHIDDevice (IOHIDFamily MatchPropertyTable owner).\n"
       "            device->setProperty(\"HIDDefaultBehavior\",\"\");\n"
       "            iface->setProperty(\"HIDDefaultBehavior\",\"\");\n")
if 'device->setProperty("HIDDefaultBehavior"' not in t:
    assert t.count(old) == 1, 'anchor'
    t = t.replace(old, new)
h.write_text(t)
print('ok')
