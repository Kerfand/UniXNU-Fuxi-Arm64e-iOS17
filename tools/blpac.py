"""blpac.py (2026-11-11): ShengBacklight's AppleARMBacklight impersonation without patching its vtable. The old impersonate() copied the
vtable, stored a raw function pointer in slot 7 and an unsigned vtable pointer in the object: fine on arm64, a pointer-authentication
data abort on arm64e (fuxi iOS 17 21H16 panicked right after "BLP: postModLoad" with sheng-backlight-control=1). getMetaClass() is now a
real override: the expansion of OSDefineMetaClassAndStructors(ShengBacklight, IOService) with only getMetaClass changed."""
from pathlib import Path
import shutil
f = Path('/home/xiaomi/sadnut-fuxi-local-20261103/device/package/AppleARMBacklight.cpp')
bak = f.with_name(f.name + '.before-pac-20261111')
if not bak.exists():
    shutil.copy2(f, bak)
t = f.read_text()
old_imp = ("        if (!gAppleClass) return false;\n"
           "        uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc(kSlots * sizeof(uintptr_t)));\n"
           "        if (!copy) return false;\n"
           "        memcpy(copy, *reinterpret_cast<uintptr_t **>(this), kSlots * sizeof(uintptr_t));\n"
           "        copy[kGetMetaClass] = reinterpret_cast<uintptr_t>(&appleMetaClass);\n"
           "        __asm__ volatile(\"dmb ish\" ::: \"memory\");\n"
           "        *reinterpret_cast<uintptr_t **>(this) = copy;\n"
           "        return true;")
new_imp = ("        // getMetaClass() (defined below, in place of OSDefineMetaClassAndStructors) answers gAppleClass from now on; no vtable\n"
           "        // copy, so arm64e's signed vtable pointer and entries stay intact.\n"
           "        return gAppleClass != nullptr;")
if old_imp in t:
    t = t.replace(old_imp, new_imp)
    t = t.replace("        enum {kSlots = 320, kGetMetaClass = 7};\n", "")
else:
    assert 'return gAppleClass != nullptr;' in t
old_def = "OSDefineMetaClassAndStructors(ShengBacklight, IOService)\n"
new_def = ("// OSDefineMetaClassAndStructors(ShengBacklight, IOService), expanded with one change: getMetaClass() answers AppleARMBacklight's\n"
           "// metaclass once impersonate() found it (IOObjectConformsTo and the sandbox's class-name check go through it). A real override is\n"
           "// what arm64e needs; patching the vtable stored unsigned pointers that pointer authentication rejects.\n"
           "ShengBacklight::MetaClass ShengBacklight::gMetaClass;\n"
           "const OSMetaClass * const ShengBacklight::metaClass = &ShengBacklight::gMetaClass;\n"
           "const OSMetaClass * const ShengBacklight::superClass = &IOService::gMetaClass;\n"
           "ShengBacklight::ShengBacklight(const OSMetaClass *meta) : IOService(meta) { }\n"
           "ShengBacklight::~ShengBacklight() { }\n"
           "const OSMetaClass *ShengBacklight::getMetaClass() const { return gAppleClass ? gAppleClass : &gMetaClass; }\n"
           "OSMetaClassConstructorInit(ShengBacklight, IOService, )\n"
           "OSDefineDefaultStructors(ShengBacklight, IOService)\n")
if old_def in t:
    assert t.count(old_def) == 1
    t = t.replace(old_def, new_def)
f.write_text(t)
print('ok', 'appleMetaClass(' in t)
