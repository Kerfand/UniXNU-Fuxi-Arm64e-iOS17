// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fuxi PMK8550 PON/RESIN and PM8550 GPIO6 -> NativeHidInput consumer report 2.
// Source: official UniXNU input guide and supplied fuxi-buttons-blob.c.
// Observer sampling plus owned peripheral type/polarity/latch writes; no PDC changes or interrupt takeover.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
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
  if(++rounds%15000==0)IOLog("[FuxiButtons] alive polls=%d reports=%d mask=%x errors=%d sink=%u\n",(int)fuxi_buttons_stats[1],(int)fuxi_buttons_stats[2],mask,(int)fuxi_buttons_stats[6],sink?1u:0u);
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
extern "C" void fuxi_buttons_probe(void){
 unsigned enabled=0;
 if(!PE_parse_boot_argn("fuxi-buttons",&enabled,sizeof(enabled))||enabled!=1||fuxi_buttons_stats[0])return;
 fuxi_buttons_stats[0]=1;
 IOLog("[FuxiButtons] enabled; observer polling begins after 60s\n");
 thread_t thread=nullptr;
 if(kernel_thread_start(buttonsThread,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
 else fail(7,-12);
}
