// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stage 1 follows https://unixnu.com/zh/porting/network: inspect firmware handoff.
// It neither powers/trains a link nor fabricates an OS network interface.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
namespace {
struct Window{IOMemoryMap *map;volatile uint32_t *va;};
bool map(Window &w,uint64_t pa,uint64_t bytes){
 auto m=IODeviceMemory::withRange(pa,bytes);w.map=m?m->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;if(m)m->release();
 w.va=w.map?reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()):nullptr;return w.va!=nullptr;
}
void release(Window &w){if(w.map)w.map->release();}
void wifiCheckThread(void *,wait_result_t){
 thread_set_thread_name(current_thread(),"fuxi-wifi-check");IOSleep(65000);
 Window gcc={},gpio={};
 if(!map(gcc,0x100000,0x80000)||!map(gpio,0xf100000,0x60000)){IOLog("[FuxiWifi] stage1 map failed\n");release(gcc);release(gpio);return;}
 const uint32_t gdsc=gcc.va[0x6b004/4];
 const uint32_t branches[]={0x6b020,0x6b02c,0x6b038,0x6b03c};uint32_t off=0;
 for(unsigned i=0;i<4;++i)if(gcc.va[branches[i]/4]&(1u<<31))off|=1u<<i;
 IOLog("[FuxiWifi] stage1 RC0 gdsc=%x clocksOff=%x vote8=%x vote0=%x wlan80=%x sw82=%x perst94=%x\n",gdsc,off,gcc.va[0x52008/4],gcc.va[0x52000/4],gpio.va[(80*0x1000+4)/4],gpio.va[(82*0x1000+4)/4],gpio.va[(94*0x1000+4)/4]);
 // Reading a collapsed/unclocked PCIe window can stall the SoC. Match all required gates first.
 if((gdsc&(1u<<31))&&!off){
  Window parf={},dbi={};
  if(map(parf,0x1c00000,0x3000)&&map(dbi,0x60000000,0x1000))
   IOLog("[FuxiWifi] RC0 rootID=%x link=%x ltssm=%x phyCtrl=%x (endpoint ID not yet read)\n",dbi.va[0],dbi.va[0x80/4],parf.va[0x1b0/4],parf.va[0x40/4]);
  release(dbi);release(parf);
 }else IOLog("[FuxiWifi] RC0 gated; skipped PARF/DBI reads; requires board power/link bring-up\n");
 IOLog("[FuxiWifi] DT variants: RC1/1103 and RC0/1107; runtime chip/firmware confirmation required, no association attempted\n");
 release(gpio);release(gcc);
}
}
extern "C" void fuxi_wifi_probe(void){
 static bool started;unsigned stage=0;
 if(!PE_parse_boot_argn("fuxi-wifi",&stage,sizeof(stage))||stage!=1||started)return;started=true;
 thread_t t=nullptr;if(kernel_thread_start(wifiCheckThread,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);
 else IOLog("[FuxiWifi] cannot create stage1 worker\n");
}
