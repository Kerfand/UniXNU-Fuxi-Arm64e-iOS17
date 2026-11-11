"""catprobe.py (2026-11-11): read-only catalogue probe. The dk=0 run shows IOHIDInterface idle (busy 0) with
DeviceUsagePairs {13,4} and no child. Once (30 s after the first no-driver checkpoint) list every catalogue
personality for this interface (IOCatalogue::findDrivers, the same set IOService::probeCandidates walks) with
its IOClass, CFBundleIdentifier, IOUserServerName and passiveMatch() result. IOCatalogue lock only; no kext
lock query, no load."""
from pathlib import Path
h = Path('/home/xiaomi/sadnut-fuxi-local-20261103/device/package/HidInputProbe.cpp')
t = h.read_text()
if '#include <IOKit/IOCatalogue.h>' not in t:
    t = t.replace('#include <IOKit/IOLib.h>\n', '#include <IOKit/IOLib.h>\n#include <IOKit/IOCatalogue.h>\n', 1)
old = "        OSSafeReleaseNULL(it);\n        appendProperty(device,\"DeviceUsagePairs\",\"DDUP\",props);\n"
new = ("        static unsigned noDriverRounds;\n"
       "        if(iface && ++noDriverRounds==30) {\n"
       "            SInt32 generation=0;\n"
       "            OSOrderedSet *set=gIOCatalogue?gIOCatalogue->findDrivers(iface,&generation):nullptr;\n"
       "            printf(\"FTS cat gen%d n%u\\n\",(int)generation,set?set->getCount():0u);\n"
       "            for(unsigned i=0;set && i<set->getCount();++i) {\n"
       "                OSDictionary *pers=OSDynamicCast(OSDictionary,set->getObject(i));\n"
       "                if(!pers)continue;\n"
       "                OSString *cls=OSDynamicCast(OSString,pers->getObject(\"IOClass\"));\n"
       "                OSString *bid=OSDynamicCast(OSString,pers->getObject(\"CFBundleIdentifier\"));\n"
       "                OSString *usr=OSDynamicCast(OSString,pers->getObject(\"IOUserServerName\"));\n"
       "                OSString *nm=OSDynamicCast(OSString,pers->getObject(\"IOPersonalityName\"));\n"
       "                bool pm=iface->passiveMatch(pers);\n"
       "                printf(\"FTS cat %u %s/%s usr%s pm%u %s\\n\",i,cls?cls->getCStringNoCopy():\"?\",\n"
       "                    bid?bid->getCStringNoCopy():\"?\",usr?\"1\":\"0\",(unsigned)pm,nm?nm->getCStringNoCopy():\"\");\n"
       "            }\n"
       "            OSSafeReleaseNULL(set);\n"
       "        }\n"
       + old)
if 'FTS cat gen' not in t:
    assert t.count(old) == 1, 'anchor'
    t = t.replace(old, new)
h.write_text(t)
print('ok')
