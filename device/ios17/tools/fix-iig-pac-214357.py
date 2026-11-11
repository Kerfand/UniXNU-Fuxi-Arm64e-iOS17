from pathlib import Path
import difflib
base=Path('/home/xiaomi/sadnut-fuxi-local-20261103')
u=base/'UniXNU'; t=u/'out-ios17/fuxi/tree'; src=t/'.cache/xnu-work'
name='iokit/DriverKit/Makefile'
original=(src/name).read_text()
anchor=next(l for l in original.splitlines(True) if '$(IIG) --def' in l)
old='SimpleMemberFunctionCast(IOService::StateNotificationItemCreate_Handler, *self, &IOService::StateNotificationItemCreate_Impl)'
new='[](OSMetaClassBase *object, OSString *name, OSDictionary *schema) -> kern_return_t { return static_cast<IOService *>(object)->StateNotificationItemCreate_Impl(name, schema); }'
cmd='ifeq ($(ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG),arm64e)\n\t@if [ "$(notdir $@)" = IOService.h ]; then '+"""python3 -c 'import sys; from pathlib import Path; p=Path(sys.argv[1]); s=p.read_text(); a=sys.argv[2]; assert s.count(a)==1; p.write_text(s.replace(a,sys.argv[3]))' "$(patsubst %.h,%.iig.cpp,$@)" """+"'"+old+"' '"+new+"'; fi\nendif\n"
replacement=original.replace(anchor,anchor+cmd)
patch=''.join(difflib.unified_diff(original.splitlines(True),replacement.splitlines(True),'a/'+name,'b/'+name))
(u/'patches/xnu-10063-arm64e/iig-state-notification-handler.patch').write_text(patch)
# Stage the rule in the warm worktree too; the baseline skips initial patches for a prepared tree.
(src/name).write_text(replacement)
(src/'iokit/DriverKit/IOService.iig').touch()
# The incremental tree already contains generated RPC code; update only the faulty expression.
p=t/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DriverKit/IOService.iig.cpp'
v=p.read_text(); assert v.count(old)==1;p.write_text(v.replace(old,new))
txt=(base/'pack-ios17-fastboot30.py').read_text().replace('fastboot30-20261105','iig-pac-20261105').replace('pack-work-ios17-fastboot30','pack-work-ios17-iig-pac')
(base/'pack-ios17-iig-pac.py').write_text(txt)