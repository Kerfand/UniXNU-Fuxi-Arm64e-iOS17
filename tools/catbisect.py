"""catbisect.py (2026-11-11): read-only. For each catalogue personality that fails passiveMatch, print its keys and
the keys whose removal (on a private copy) makes passiveMatch succeed."""
from pathlib import Path
h = Path('/home/xiaomi/sadnut-fuxi-local-20261103/device/package/HidInputProbe.cpp')
t = h.read_text()
old = ("                printf(\"FTS cat %u %s/%s usr%s pm%u %s\\n\",i,cls?cls->getCStringNoCopy():\"?\",\n"
       "                    bid?bid->getCStringNoCopy():\"?\",usr?\"1\":\"0\",(unsigned)pm,nm?nm->getCStringNoCopy():\"\");\n")
new = old + (
       "                if(!pm && !usr) {\n"
       "                    char keys[400];size_t kl=0;keys[0]=0;\n"
       "                    char flips[200];size_t fl=0;flips[0]=0;\n"
       "                    OSCollectionIterator *ki=OSCollectionIterator::withCollection(pers);\n"
       "                    while(ki) {\n"
       "                        OSSymbol *key=OSDynamicCast(OSSymbol,ki->getNextObject());\n"
       "                        if(!key)break;\n"
       "                        int n=snprintf(keys+kl,sizeof keys-kl,\"%s,\",key->getCStringNoCopy());\n"
       "                        if(n>0 && (size_t)n<sizeof keys-kl)kl+=n;\n"
       "                        OSDictionary *copy=OSDictionary::withDictionary(pers);\n"
       "                        if(copy) {\n"
       "                            copy->removeObject(key);\n"
       "                            if(iface->passiveMatch(copy)) {\n"
       "                                n=snprintf(flips+fl,sizeof flips-fl,\"%s,\",key->getCStringNoCopy());\n"
       "                                if(n>0 && (size_t)n<sizeof flips-fl)fl+=n;\n"
       "                            }\n"
       "                            copy->release();\n"
       "                        }\n"
       "                    }\n"
       "                    OSSafeReleaseNULL(ki);\n"
       "                    printf(\"FTS cat %u keys %s\\nFTS cat %u flip %s\\n\",i,keys,i,flips[0]?flips:\"none\");\n"
       "                }\n")
if 'FTS cat %u flip' not in t:
    assert t.count(old) == 1, 'anchor'
    t = t.replace(old, new)
    if '#include <libkern/c++/OSCollectionIterator.h>' not in t:
        t = t.replace('#include <libkern/c++/OSIterator.h>\n', '#include <libkern/c++/OSIterator.h>\n#include <libkern/c++/OSCollectionIterator.h>\n#include <libkern/c++/OSDictionary.h>\n', 1)
h.write_text(t)
print('ok')
