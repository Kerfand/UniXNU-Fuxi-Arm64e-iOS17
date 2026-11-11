"""keychains.py (2026-11-11): the scratch Data seed has no /private/var/Keychains. securityd runs as _securityd and cannot
create it under root-owned /private/var, so it never opens the keychain database: every SecItem query fails before any
AppleKeyStore call (hardware: securityd made no IOKit calls), and mobileactivationd dies in its first-boot data migration
("Failed to query keychain"). On a device the directory comes with the data volume, owned by _securityd (UniXNU docs,
devices/fajita: "Data 种子把 Keychains 交给 _securityd"). boot-arg sheng-keychains=1: after the Data mount, create it
(0755, uid 64 _securityd, gid 0) if missing. Gated; nothing changes without the argument."""
from pathlib import Path
R = Path('/home/xiaomi/sadnut-fuxi-local-20261103')
f = R / 'device/package/BootWatchdogPolicy.c'
t = f.read_text()
helper_anchor = "// One comma-separated label at *c into the policy as a disabled key; advances *c past it.\n"
helper = (
"// sheng-keychains=1: /private/var/Keychains for securityd (uid 64 _securityd, gid 0, 0755) when the Data seed lacks it.\n"
"// securityd cannot create it itself under root-owned /private/var, so without it no keychain item can be stored or queried.\n"
"static int sheng_boot_keychains(proc_t p,uint64_t scratch) {\n"
" int on=0;int32_t result=0;\n"
" if(!PE_parse_boot_argn(\"sheng-keychains\",&on,sizeof(on)) || on!=1)return 0;\n"
" const char *dir=\"/private/var/Keychains\";\n"
" int error=copyout(dir,scratch,strlen(dir)+1);\n"
" if(error)return error;\n"
" struct mkdir_args md={.path=scratch,.mode=0755};\n"
" error=mkdir(p,&md,&result);\n"
" int created=!error;\n"
" if(error && error!=EEXIST)return error;\n"
" struct lchown_args own={.path=scratch,.owner=64,.group=0};\n"
" error=lchown(p,&own,&result);\n"
" if(!error){struct chmod_args cm={.path=scratch,.mode=0755};error=chmod(p,&cm,&result);}\n"
" printf(\"sheng keychains: %s rc %d\\n\",created?\"created\":\"existing\",error);\n"
" return error;\n"
"}\n\n")
if 'sheng_boot_keychains' not in t:
    assert t.count(helper_anchor) == 1
    t = t.replace(helper_anchor, helper + helper_anchor)
call_anchor = " if(setup==1)printf(\"sheng setup flags reset: Hello page on this boot\\n\");\n"
call = call_anchor + " // Independent of the launch policy below: report and continue on failure.\n (void)sheng_boot_keychains(p,scratch);\n"
if '(void)sheng_boot_keychains' not in t:
    assert t.count(call_anchor) == 1
    t = t.replace(call_anchor, call)
f.write_text(t)
print('policy ok')
pk = R / 'pack-ios17-hidpath.py'
s = pk.read_text()
if 'sheng-keychains=1' not in s:
    o = "        + 'fuxi-buttons=1 '\n"
    assert s.count(o) == 1
    s = s.replace(o, o + "        # /private/var/Keychains for securityd (BootWatchdogPolicy.c): the Data seed lacks it, so no keychain query can succeed.\n"
                         "        + 'sheng-keychains=1 '\n")
    pk.write_text(s)
print('pack ok')
