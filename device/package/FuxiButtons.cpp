// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fuxi PMK8550 PON/RESIN and PM8550 GPIO6 -> NativeHidInput consumer report 2.
// Source: official UniXNU input guide and supplied fuxi-buttons-blob.c.
// Observer sampling plus owned peripheral type/polarity/latch writes; no PDC changes or interrupt takeover.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
extern "C" void unixnu_sample_task(const char *name) __attribute__((weak_import));
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include "FuxiButtonsCore.h"
extern "C" {
#include "BootResetCore.h"
volatile int32_t fuxi_buttons_stats[10];
// One owner of the SPMI arbiter at a time (the mainline driver serialises every transaction under pmic_arb->lock); FuxiWifiLink takes it too.
volatile uint32_t fuxi_spmi_lock_word;
void fuxi_spmi_lock(void){while(__atomic_exchange_n(&fuxi_spmi_lock_word,1u,__ATOMIC_ACQUIRE))IODelay(1);}
void fuxi_spmi_unlock(void){__atomic_store_n(&fuxi_spmi_lock_word,0u,__ATOMIC_RELEASE);}
}
extern "C" void clock_set_calendar_microtime(clock_sec_t secs, clock_usec_t microsecs);
volatile uint32_t fuxi_rtc_stats[4];   // 0 state (1 read, 2 set, <0 failed step), 1 seconds, 2 control, 3 apid
extern "C" int vn_rdwr(enum uio_rw rw, struct vnode *vp, caddr_t base, int len, off_t offset, enum uio_seg segflg, int ioflg,
    kauth_cred_t cred, int *aresid, proc_t p);
namespace {
constexpr uint64_t kCore=0xc400000,kCnfg=0xc42d000,kObserver=0xc440000;
struct Window {IOMemoryMap *map;volatile uint32_t *va;};
Window gCore,gCnfg,gObs,gChn;
bool gEdgeLatch;
br_arbiter gArb;
int gPon=-1,gGpio=-1;
void (*volatile gSink)(void *,uint32_t);
void *volatile gSinkContext;
volatile uint32_t gSinkGeneration;
bool mapWindow(Window &w,uint64_t base,uint64_t size){
 IODeviceMemory *m=IODeviceMemory::withRange(base,size);
 w.map=m?m->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
 if(m)m->release();
 w.va=w.map?reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()):nullptr;
 return w.va!=nullptr;
}
void fail(int step,int error){
 fuxi_buttons_stats[0]=-step;fuxi_buttons_stats[8]=error;
 IOLog("[FuxiButtons] setup failed step=%d error=%d\n",step,error);
}
// Restrict access to an EE0-owned APID. Read access does not require the IRQ-owner bit.
int findApid(uint32_t ppid,uint32_t count){
 int found=-1;
 for(uint32_t i=0;i<count;++i){
  uint32_t map=gCore.va[(BR_APID_MAP+4*i)/4];
  if(map && ((map>>8)&0xfff)==ppid && (gCnfg.va[i]&7u)==gArb.ee){
   if(found<0 || (map&(1u<<24)))found=(int)i;
  }
 }
 return found;
}
// The system clock starts at 1970 (no calendar source): HTTPS certificate checks fail, so Wi-Fi joins but nothing loads.
// PMK8550 RTC (fuxi 2026-11-14): read once; the calendar is set only from a plausible Unix time, otherwise only logged.
int gRtcApid=-1;
volatile int64_t gPendingOffset;
volatile bool gOffsetPending;
const char kOffsetPath[]="/private/var/db/fuxi-rtc-offset";
// Seconds of the RTC (RTC_RW READ 0x48..0x4b, little endian), read until the low byte did not wrap. Caller holds no lock.
int rtcSeconds(uint32_t *secs,uint8_t *ctrl){
 if(gRtcApid<0)return -1;
 uint8_t b[4]={},c0=0;int rc=0;
 fuxi_spmi_lock();
 rc=br_read8(gObs.va,&gArb,gRtcApid,0x46,ctrl);
 for(int tries=0;tries<3 && !rc;++tries){
  for(int i=0;i<4 && !rc;++i)rc=br_read8(gObs.va,&gArb,gRtcApid,(uint8_t)(0x48+i),&b[i]);
  if(!rc)rc=br_read8(gObs.va,&gArb,gRtcApid,0x48,&c0);
  if(!rc && c0>=b[0])break;
 }
 fuxi_spmi_unlock();
 *secs=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
 return rc;
}
// The stored offset (calendar seconds minus RTC seconds) as decimal text on the Data volume.
bool offsetFile(bool write,int64_t *offset){
 vfs_context_t ctx=vfs_context_create(nullptr);
 if(!ctx)return false;
 vnode_t vp=nullptr;
 // vnode_open takes FREAD/FWRITE (1/2), not O_RDONLY/O_WRONLY (0/1): with O_WRONLY the file was opened as a reader and
 // vnode_close(FWRITE) took the write count to -1, a panic (vnode_rele_ext ... writecount -ve) the first time the network set
 // the time after a Wi-Fi join (2026-11-18).
 int error=vnode_open(kOffsetPath,write?(FWRITE|O_CREAT|O_TRUNC|O_NOFOLLOW):(FREAD|O_NOFOLLOW),0644,0,&vp,ctx);
 bool ok=false;
 if(!error){
  char text[32]={};int resid=0;
  if(write){
   int n=snprintf(text,sizeof text,"%lld\n",(long long)*offset);
   error=vn_rdwr(UIO_WRITE,vp,text,n,0,UIO_SYSSPACE,IO_NOAUTH|IO_UNIT,vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx));
   ok=!error && !resid;
  }else{
   error=vn_rdwr(UIO_READ,vp,text,sizeof text-1,0,UIO_SYSSPACE,IO_NOAUTH,vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx));
   if(!error){
    int64_t v=0;bool neg=false;const char *c=text;
    if(*c=='-'){neg=true;++c;}
    bool digits=false;while(*c>='0'&&*c<='9'){v=v*10+(*c-'0');++c;digits=true;}
    if(digits){*offset=neg?-v:v;ok=true;}
   }
  }
  vnode_close(vp,write?FWRITE:FREAD,ctx);
 }
 vfs_context_rele(ctx);
 if(!ok)IOLog("fuxi rtc: offset file %s error %d\n",write?"write":"read",error);
 return ok;
}
void readRtc(uint32_t count){
 // Observer (read) channels need no ownership: the RTC_RW APID belongs to another EE, so findApid (EE0 only) misses it.
 int apid=-1;
 for(uint32_t i=0;i<count;++i){
  uint32_t map=gCore.va[(BR_APID_MAP+4*i)/4];
  if(map && ((map>>8)&0xff)==0x61)IOLog("fuxi rtc: candidate apid %u ppid %#x owner %u\n",i,(map>>8)&0xfff,gCnfg.va[i]&7u);
  if(map && ((map>>8)&0xfff)==0x061 && apid<0)apid=(int)i;
 }
 fuxi_rtc_stats[3]=(uint32_t)apid;
 if(apid<0){fuxi_rtc_stats[0]=(uint32_t)-1;IOLog("fuxi rtc: no RTC_RW apid\n");return;}
 gRtcApid=apid;
 uint32_t secs=0;uint8_t ctrl=0;
 int rc=rtcSeconds(&secs,&ctrl);
 fuxi_rtc_stats[1]=secs;fuxi_rtc_stats[2]=ctrl;
 if(rc){fuxi_rtc_stats[0]=(uint32_t)-2;IOLog("fuxi rtc: read error %d\n",rc);return;}
 // fuxi 2026-11-17: a static offset when there is no stored one. calendar - RTC measured on three boots of 2026-11-17 (USB log capture
 // start in UTC against the "fuxi rtc" reading: 1794549623, ...629, ...624; the reading comes a few seconds after the capture opens),
 // so the clock is right from the first boot without Settings or the network. Order: boot argument fuxi-time-offset=<seconds>, the
 // offset Settings stored on Data, this constant.
 // 2026-11-18: the network time after a Wi-Fi join gave calendar - RTC = 1794549681 ("fuxi rtc: calendar set to 1795000399,
 // rtc 450718"); the USB-capture estimate above was 54 s slow.
 static const int64_t kStaticRtcOffset=1794549681;
 int64_t offset=0;
 const char *source="static";
 uint64_t arg=0;
 if(PE_parse_boot_argn("fuxi-time-offset",&arg,sizeof(arg))&&arg){offset=(int64_t)arg;source="boot-arg";}
 else if(offsetFile(false,&offset))source="stored";
 else offset=kStaticRtcOffset;
 const bool stored=true;
 // 2026-11-19: the PMK8550 RTC does not count Unix time; each phone's counter starts from its own point, so the static offset above
 // is right only on the phone it was measured on. On another phone the result is accepted only between this build's date and two
 // years later; otherwise the calendar starts at the build date (TLS certificates then validate) until the network time or Settings
 // sets it, which stores this phone's own offset for later boots.
 static const int64_t kBuildFloor=1795046400;   // 2026-11-19 00:00 UTC
 int64_t calendar=(int64_t)secs+offset;
 bool plausible=(ctrl&0x80) && calendar>=kBuildFloor-86400 && calendar<kBuildFloor+2*31536000LL;
 if(!plausible && strcmp(source,"static")==0){calendar=kBuildFloor;plausible=true;source="build-date";}
 if(plausible)clock_set_calendar_microtime((clock_sec_t)calendar,0);
 fuxi_rtc_stats[0]=plausible?2u:1u;
 (void)stored;
 IOLog("fuxi rtc: apid %d ctrl %#x rtc %u offset %lld (%s) calendar %lld -> %s\n",apid,(unsigned)ctrl,secs,(long long)offset,
     source,(long long)calendar,plausible?"calendar set":"left alone");
}
bool setup(){
 gArb.ee=0;
 if(!mapWindow(gCore,kCore,0x3000)||!mapWindow(gCnfg,kCnfg,0x4000)){fail(1,-12);return false;}
 gArb.core=gCore.va;gArb.cnfg=gCnfg.va;
 uint32_t version=gCore.va[0],count=gCore.va[BR_FEATURES/4]&BR_FEATURES_APIDS_MASK;
 if(version<BR_VERSION_V7_MIN||version>=BR_VERSION_V8_MIN||!count||count>1024){fail(2,(int)version);return false;}
 if(!mapWindow(gObs,kObserver,((uint64_t)BR_OBS_APID_STRIDE*count+0xfff)&~0xfffull)){fail(3,-12);return false;}
 gPon=findApid(0x013,count);gGpio=findApid(0x18d,count);
 fuxi_buttons_stats[4]=gPon;fuxi_buttons_stats[5]=gGpio;
 if(gPon<0||gGpio<0){fail(4,-2);return false;}
 // Configure only our interrupt type/polarity/latch registers. Do not enable
 // or reroute an IRQ: latch polling needs no PDC/GIC ownership transfer.
 if(mapWindow(gChn,0xc500000,(uint64_t)BR_CHNL_APID_STRIDE*count)){
  int rc=0;fuxi_spmi_lock();
  for(unsigned key=0;key<2 && !rc;++key){
   int apid=key?gGpio:gPon;uint8_t bits=key?1:0xc0;
   for(uint8_t reg=0x11;reg<=0x13 && !rc;++reg){uint8_t v=0;rc=br_read8(gObs.va,&gArb,apid,reg,&v);
    if(!rc)rc=br_write8(gChn.va,apid,reg,(uint8_t)(v|bits));}
   if(!rc)rc=br_write8(gChn.va,apid,0x14,bits);
  }
  fuxi_spmi_unlock();gEdgeLatch=rc==0;
  IOLog("[FuxiButtons] edge latch setup rc=%d active=%u (no IRQ enable/routing writes)\n",rc,gEdgeLatch?1u:0u);
 }
 fuxi_buttons_stats[0]=3;
 IOLog("[FuxiButtons] ready mode=edge-poll2ms version=%x apids=%u pon=%d gpio=%d\n",version,count,gPon,gGpio);
 readRtc(count);
 return true;
}
void buttonsThread(void *,wait_result_t){
 thread_set_thread_name(current_thread(),"fuxi-buttons");
 // The supplied fuxi bring-up notes require the firmware handoff to settle before SPMI reads.
 IOSleep(60000);
 if(!setup())return;
 FuxiButtonsState state={};uint32_t last=0,seen=0,rounds=0;
 for(;;){
  uint8_t pon=0,gpio=0xff;
  fuxi_spmi_lock();
  int rc=br_read8(gObs.va,&gArb,gPon,0x10,&pon);
  if(!rc)rc=br_read8(gObs.va,&gArb,gGpio,0x10,&gpio);
  uint8_t lp=0,lg=0;int er=-1;
  if(!rc && gEdgeLatch){
   er=br_read8(gObs.va,&gArb,gPon,0x18,&lp);
   if(!er)er=br_read8(gObs.va,&gArb,gGpio,0x18,&lg);
   if(!er && (lp&0xc0))er=br_write8(gChn.va,gPon,0x14,(uint8_t)(lp&0xc0));
   if(!er && (lg&1))er=br_write8(gChn.va,gGpio,0x14,(uint8_t)(lg&1));
  }
  fuxi_spmi_unlock();
  fuxi_buttons_stats[1]=(int32_t)((uint32_t)fuxi_buttons_stats[1]+1u);fuxi_buttons_stats[8]=rc;
  if(rc){fuxi_buttons_stats[6]=(int32_t)((uint32_t)fuxi_buttons_stats[6]+1u);if(state.errors==0)IOLog("[FuxiButtons] read error=%d (release after 3 failed polls)\n",rc);}
  else{fuxi_buttons_stats[7]=(int32_t)((uint32_t)pon|((uint32_t)gpio<<8));}
  uint32_t edges=0;
  if(!rc && gEdgeLatch){
   if(er){gEdgeLatch=false;IOLog("[FuxiButtons] edge latch disabled after error=%d; live sampling continues\n",er);}
   else edges=((lp&0x80)?2u:0u)|((lp&0x40)?8u:0u)|((lg&1)?4u:0u);
  }
  uint32_t mask=fuxi_buttons_sample(&state,fuxi_buttons_decode(pon,gpio),rc==0,edges);
  fuxi_buttons_stats[3]=(int32_t)mask;
  uint32_t generation=__atomic_load_n(&gSinkGeneration,__ATOMIC_ACQUIRE);
  auto sink=__atomic_load_n(&gSink,__ATOMIC_ACQUIRE);
  if(sink && (mask!=last||generation!=seen)){
   sink(gSinkContext,mask);fuxi_buttons_stats[2]=(int32_t)((uint32_t)fuxi_buttons_stats[2]+1u);
   IOLog("[FuxiButtons] report mask=%x pon=%x gpio=%x error=%d\n",mask,(unsigned)pon,(unsigned)gpio,rc);
   last=mask;seen=generation;
  }
  if((rounds+1)%15000==0 && unixnu_sample_task)unixnu_sample_task("bluetoothd");
  if(++rounds%15000==0)IOLog("[FuxiButtons] alive polls=%d reports=%d mask=%x errors=%d sink=%u\n",(int)fuxi_buttons_stats[1],(int)fuxi_buttons_stats[2],mask,(int)fuxi_buttons_stats[6],sink?1u:0u);
  if(__atomic_load_n(&gOffsetPending,__ATOMIC_ACQUIRE)){
   __atomic_store_n(&gOffsetPending,false,__ATOMIC_RELEASE);
   int64_t offset=gPendingOffset;
   const bool ok=offsetFile(true,&offset);
   IOLog("fuxi rtc: offset %lld %s\n",(long long)offset,ok?"stored":"not stored");
  }
  IOSleep(2);
 }
}
}
extern "C" void native_buttons_set_sink(void (*sink)(void *,uint32_t),void *context){
 __atomic_store_n(&gSink,nullptr,__ATOMIC_RELEASE);
 gSinkContext=context;
 __atomic_store_n(&gSink,sink,__ATOMIC_RELEASE);
 __atomic_fetch_add(&gSinkGeneration,1,__ATOMIC_RELEASE);
}
// zz-calendar-hook.patch: a user-set calendar (Settings, timed) stores offset = calendar - RTC for the next boots.
extern "C" void unixnu_calendar_set_hook(uint64_t seconds){
 uint32_t secs=0;uint8_t ctrl=0;
 if(rtcSeconds(&secs,&ctrl)){IOLog("fuxi rtc: calendar set to %llu before the RTC is ready; offset not stored\n",(unsigned long long)seconds);return;}
 // The setting process is sandboxed (its context may not write /private/var/db): the buttons kernel thread stores the offset.
 gPendingOffset=(int64_t)seconds-(int64_t)secs;
 __atomic_store_n(&gOffsetPending,true,__ATOMIC_RELEASE);
 IOLog("fuxi rtc: calendar set to %llu, rtc %u, offset %lld queued\n",(unsigned long long)seconds,secs,(long long)gPendingOffset);
}
extern "C" void fuxi_buttons_probe(void){
 unsigned enabled=0;
 if(!PE_parse_boot_argn("fuxi-buttons",&enabled,sizeof(enabled))||enabled!=1||fuxi_buttons_stats[0])return;
 fuxi_buttons_stats[0]=1;
 IOLog("[FuxiButtons] enabled; observer polling begins after 60s\n");
 thread_t thread=nullptr;
 if(kernel_thread_start(buttonsThread,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
 else fail(7,-12);
}

// fuxi USB control (INPUT verb, fuxi-usb-control-20261112). op 0: availability (no side effect); 1/2: touch (FuxiTouchProbe);
// 3: buttons mask a through the same sink as the polled PMIC keys (bit 0 menu/home, 1 power, 2 volume up, 3 volume down,
// HID usages 0x40/0x30/0xe9/0xea), ORed with the keys physically held; 0 releases. Runs in the USB worker thread.
extern "C" int fuxi_control_touch(unsigned op, unsigned a, unsigned b);
extern "C" int fuxi_control_input(unsigned op, unsigned a, unsigned b) {
 if(op==0)return 1;
 if(op==1||op==2)return fuxi_control_touch(op,a,b);
 if(op==3){
  auto sink=__atomic_load_n(&gSink,__ATOMIC_ACQUIRE);
  if(!sink || a>15u)return 0;
  sink(gSinkContext,(a|(uint32_t)fuxi_buttons_stats[3])&15u);
  IOLog("[FuxiButtons] usb control mask=%x\n",a);
  return 1;
 }
 return 0;
}
