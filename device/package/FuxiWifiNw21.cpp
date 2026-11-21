// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fuxi board side of the Ath12kWifi module at f6bf561, the in-kernel station runtime UniXNU-sheng ran on iPad7,1 iPadOS 17.7.11 (21H461) with
// WifiProbe.cpp stage 6 and WifiNet.cpp at a57341b: WPA2-Personal, DHCP and traffic verified there (sheng porting.toml, 08cde20). The module
// does its own chip power-up, MHI boot, QRTR/QMI, WPA2 handshake and station; this file gives it register access, configuration space, the
// PCIe0 MSI line, DMA memory from the endpoint's SMMU window (FuxiWifiMhi.cpp dmaPrepare), the embedded firmware (FuxiWifiFwStore.cpp) and a
// log, then starts the network side (FuxiWifiNet.cpp: en0, the registry entries and the Apple80211 requests wifid makes) once the module is up.
// Same entry as the mainline board file (FuxiWifiNw.cpp); the build links one of them (FUXI_WIFI_RUNTIME).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <stdarg.h>
#include "nw_abi.h"
extern "C" int armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *) __attribute__((weak_import));
extern "C" int fuxi_wifi_firmware_get(const char *,const uint8_t **,size_t *);   // FuxiWifiFwStore.cpp
extern kmod_info_t g_kernel_kmod_info;
// FuxiWifiNet.cpp: its static initializer (the metaclasses of its registry classes) is taken out of __mod_init_func by the build and run here,
// between preModLoad and postModLoad, as sheng does for WifiNet.cpp.
extern "C" void fuxi_wifi_net_static_init(void) __asm__("__GLOBAL__sub_I_FuxiWifiNet.cpp");
extern "C" void fuxi_wifi_net_start(const uint8_t mac[6]);
namespace {
volatile uint32_t *gCfg,*gDbi,*gBar;
volatile uint32_t *gFault;volatile uint64_t *gFar;
unsigned gLogLevel=6;

// ---- log: one IOLog line per message on the USB console; a line equal to one of the last sixteen is only counted ----
uint32_t gLogRepeats,gDebugLines;
bool has(const char *s,const char *w){for(;*s;++s){size_t k=0;while(w[k]&&s[k]==w[k])++k;if(!w[k])return true;}return false;}
uint64_t pNow(){uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000;}
void pLog(int level,const char *line){
 if(level>(int)gLogLevel)return;
 if(level>=7&&++gDebugLines>4000)return;
 if(has(line,"chan scan list")||has(line,"mac channel [")){++gLogRepeats;return;}
 if(level>=7&&(has(line,"htc rx completion")||has(line,"rx ce pipe")||has(line,"ext irq:")||has(line,"event mgmt rx")||
               has(line,"mgmt rx event")||has(line,"chan info vdev_id")||has(line,"scan event foreign channel"))){++gLogRepeats;return;}
 static uint32_t recent[16];static uint64_t first;
 uint32_t h=2166136261u;for(const char *c=line;*c;++c)h=(h^static_cast<uint8_t>(*c))*16777619u;h^=static_cast<uint32_t>(level);
 for(uint32_t r:recent)if(r==h){++gLogRepeats;return;}
 memmove(&recent[1],&recent[0],sizeof recent-sizeof recent[0]);recent[0]=h;
 const uint64_t now=pNow();if(!first)first=now;
 IOLog("[FuxiNw] %u %d %.150s\n",(unsigned)(now-first),level,line);
}
void nwLog(int level,const char *fmt,...){char line[180];va_list ap;va_start(ap,fmt);vsnprintf(line,sizeof line,fmt,ap);va_end(ap);pLog(level,line);}

// ---- DMA: carved out of the SMMU window, first page the MSI target, buffers aligned to their size rounded up to a power of two ----
constexpr unsigned kArenaBlocks=16384;
struct Arena {uint8_t *va;uint64_t iova;size_t bytes;struct Block {uint32_t off,size;bool used;} *blocks;unsigned count;size_t used,peak;unsigned failures;} gArena;
bool arenaInit(uint8_t *va,uint64_t iova,size_t bytes){
 if(!va||bytes<(1u<<20))return false;
 if(!gArena.blocks)gArena.blocks=static_cast<Arena::Block *>(IOMallocData(kArenaBlocks*sizeof(Arena::Block)));
 if(!gArena.blocks)return false;
 gArena.va=va;gArena.iova=iova;gArena.bytes=bytes;
 gArena.blocks[0]={4096,(uint32_t)(bytes-4096),false};gArena.count=1;
 gArena.used=gArena.peak=4096;gArena.failures=0;
 return true;
}
void *arenaAlloc(size_t n,uint64_t *bus){
 n=(n+63)&~(size_t)63;
 if(n>=4096)n=(n+4095)&~(size_t)4095;
 uint32_t align=64;
 if(n>=4096){align=4096;while(align<n)align<<=1;}
 for(unsigned i=0;i<gArena.count;++i){
  Arena::Block &b=gArena.blocks[i];
  if(b.used)continue;
  const uint32_t start=(b.off+align-1)&~(align-1),pad=start-b.off;
  if(b.size<pad+n)continue;
  const bool tail=b.size>pad+n;
  const unsigned extra=(pad?1u:0u)+(tail?1u:0u);
  if(gArena.count+extra>kArenaBlocks)continue;
  const Arena::Block whole=b;
  memmove(&gArena.blocks[i+extra],&gArena.blocks[i],(gArena.count-i)*sizeof(Arena::Block));
  gArena.count+=extra;
  unsigned at=i;
  if(pad){gArena.blocks[at]={whole.off,pad,false};++at;}
  gArena.blocks[at]={start,(uint32_t)n,true};
  if(tail)gArena.blocks[at+1]={start+(uint32_t)n,whole.size-pad-(uint32_t)n,false};
  uint8_t *p=gArena.va+start;
  gArena.used+=n;if(gArena.used>gArena.peak)gArena.peak=gArena.used;
  bzero(p,n);
  if(bus)*bus=gArena.iova+start;
  return p;
 }
 ++gArena.failures;
 nwLog(3,"dma window full: want %zu used %zu of %zu blocks %u",n,gArena.used,gArena.bytes,gArena.count);
 return nullptr;
}
void arenaFree(void *p){
 if(!p||(uint8_t *)p<gArena.va||(uint8_t *)p>=gArena.va+gArena.bytes)return;
 const uint32_t off=(uint32_t)((uint8_t *)p-gArena.va);
 for(unsigned i=0;i<gArena.count;++i){
  if(gArena.blocks[i].off!=off||!gArena.blocks[i].used)continue;
  gArena.used-=gArena.blocks[i].size;
  gArena.blocks[i].used=false;
  if(i+1<gArena.count&&!gArena.blocks[i+1].used){gArena.blocks[i].size+=gArena.blocks[i+1].size;memmove(&gArena.blocks[i+1],&gArena.blocks[i+2],(gArena.count-i-2)*sizeof(Arena::Block));--gArena.count;}
  if(i>0&&!gArena.blocks[i-1].used){gArena.blocks[i-1].size+=gArena.blocks[i].size;memmove(&gArena.blocks[i],&gArena.blocks[i+1],(gArena.count-i-1)*sizeof(Arena::Block));--gArena.count;}
  return;
 }
}
// fuxi 2026-11-20: packet buffers (nw_net.c alloc_skb through fuxi_wifi_dma_alloc_stream) come from gStream, the top kStreamBytes of the
// window reached through its cached alias (FuxiWifiMhi.cpp fuxi_wifi_dma_cached_alias): the stack copied every packet byte by byte from
// write-combining memory and searched the whole block list for every allocation, so a 1000 Mbps link measured under 100 Mbps. gStream
// is a slab: size classes of 512 B .. 64 KiB, each block a 64-byte CPU-only header (its class) and then the buffer, freed blocks kept
// on their class's list. That part of the window is never touched through the write-combining mapping. Every map and sync of a gStream
// buffer does the cache maintenance: clean before the device reads it, clean+invalidate before and after the device writes it. Rings and
// all other coherent allocations stay in gArena. fuxi-wifi-cached=0 keeps the whole window write-combining as before.
constexpr size_t kStreamBytes=64u<<20;
constexpr unsigned kStreamClasses=8;   // 512 << class
struct Stream {uint8_t *va;uint64_t iova;size_t bytes,next;void *free[kStreamClasses];unsigned live,failures;} gStream;
bool gStreamOn;
IOSimpleLock *gStreamLock;
inline bool inStream(const void *p){return gStreamOn&&(const uint8_t *)p>=gStream.va&&(const uint8_t *)p<gStream.va+gStream.bytes;}
inline bool busInStream(uint64_t bus){return gStreamOn&&bus>=gStream.iova&&bus<gStream.iova+gStream.bytes;}
void cacheLines(const uint8_t *p,size_t n,bool invalidate){
 uintptr_t a=(uintptr_t)p&~(uintptr_t)63;const uintptr_t end=(uintptr_t)p+n;
 if(invalidate)for(;a<end;a+=64)__asm__ volatile("dc civac, %0"::"r"(a):"memory");
 else for(;a<end;a+=64)__asm__ volatile("dc cvac, %0"::"r"(a):"memory");
 __asm__ volatile("dsb sy":::"memory");
}
void *streamAlloc(size_t n,uint64_t *bus){
 unsigned c=0;while(c<kStreamClasses&&(512u<<c)<n)++c;
 if(c==kStreamClasses)return nullptr;
 const size_t block=64+(512u<<c);
 IOSimpleLockLock(gStreamLock);
 uint8_t *h=static_cast<uint8_t *>(gStream.free[c]);
 if(h)gStream.free[c]=*reinterpret_cast<void **>(h+8);
 else if(gStream.next+block<=gStream.bytes){h=gStream.va+gStream.next;gStream.next+=block;}
 if(h)++gStream.live;else ++gStream.failures;
 IOSimpleLockUnlock(gStreamLock);
 if(!h)return nullptr;
 *reinterpret_cast<uint32_t *>(h)=c;
 if(bus)*bus=gStream.iova+(uint64_t)(h+64-gStream.va);
 return h+64;
}
void streamFree(void *p){
 uint8_t *h=static_cast<uint8_t *>(p)-64;
 const uint32_t c=*reinterpret_cast<uint32_t *>(h);
 if(c>=kStreamClasses)return;
 IOSimpleLockLock(gStreamLock);
 *reinterpret_cast<void **>(h+8)=gStream.free[c];gStream.free[c]=h;--gStream.live;
 IOSimpleLockUnlock(gStreamLock);
}
extern "C" uint8_t *fuxi_wifi_dma_cached_alias(const uint8_t *wc);
// Called before arenaInit: takes the top of the window for gStream and returns the bytes left for gArena.
size_t streamInit(uint8_t *va,uint64_t iova,size_t bytes){
 unsigned want=1;PE_parse_boot_argn("fuxi-wifi-cached",&want,sizeof want);
 uint8_t *cached=want==1?fuxi_wifi_dma_cached_alias(va):nullptr;
 if(cached&&bytes>=2*kStreamBytes&&(gStreamLock=IOSimpleLockAlloc())){
  gStream.va=cached+(bytes-kStreamBytes);gStream.iova=iova+(bytes-kStreamBytes);gStream.bytes=kStreamBytes;gStream.next=0;
  gStreamOn=true;bytes-=kStreamBytes;
 }
 IOLog("[FuxiNw] packet buffers: %s\n",gStreamOn?"cached alias, 64 MiB slab":"write-combining window");
 return bytes;
}
void *pDmaAlloc(size_t n,uint64_t *bus){return arenaAlloc(n,bus);}
void pDmaFree(size_t,void *p,uint64_t){if(inStream(p))streamFree(p);else arenaFree(p);}
uint64_t pDmaMap(void *p,size_t n,int dir){
 if(inStream(p)){
  // Handing a packet buffer to the device: write back what the CPU wrote; for a receive buffer also drop the lines, so no dirty line can
  // be evicted over the device's data later.
  cacheLines(static_cast<const uint8_t *>(p),n,dir!=1 /* DMA_TO_DEVICE */);
  return gStream.iova+(uint64_t)((uint8_t *)p-gStream.va);
 }
 if((uint8_t *)p<gArena.va||(uint8_t *)p>=gArena.va+gArena.bytes)return 0;
 return gArena.iova+((uint8_t *)p-gArena.va);
}
void pDmaSync(uint64_t bus,size_t n,int dir){
 if(busInStream(bus)){cacheLines(gStream.va+(bus-gStream.iova),n,dir!=1);return;}
 __asm__ volatile("dsb sy":::"memory");   // the CPU sees the rest of the window write-combined (normal non-cacheable)
}
void pSleep(unsigned ms){IOSleep(ms);}
void pPoll(){}
void pHalt(){for(;;)IOSleep(1000);}
void *hAlloc(size_t n){
 uint8_t *p=static_cast<uint8_t *>(IOMallocData(n+16));
 if(!p)return nullptr;
 *reinterpret_cast<size_t *>(p)=n+16;
 return p+16;
}
void hFree(void *q){
 if(!q)return;
 uint8_t *p=static_cast<uint8_t *>(q)-16;
 IOFreeData(p,*reinterpret_cast<size_t *>(p));
}

// ---- the radio's address: made from /chosen/unique-chip-id (this firmware reports none), handed to the module as wlan_mac.bin and left
// in the device tree where the wireless daemon reads it (sheng: wifid derives every network's private address from it and aborts a join
// without it) ----
bool boardMac(uint8_t m[6]){
 IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
 OSData *id=chosen?OSDynamicCast(OSData,chosen->getProperty("unique-chip-id")):nullptr;
 uint64_t seed=0;
 if(id&&id->getLength()>=sizeof seed)memcpy(&seed,id->getBytesNoCopy(),sizeof seed);
 OSSafeReleaseNULL(chosen);
 if(!seed)return false;
 uint64_t h=0xcbf29ce484222325ull;
 for(unsigned i=0;i<8;++i)h=(h^((seed>>(8*i))&0xff))*0x100000001b3ull;
 for(unsigned i=0;i<6;++i)m[i]=(uint8_t)(h>>(8*i));
 m[0]=(uint8_t)((m[0]&0xfe)|0x02);
 return true;
}
void publishMac(){
 uint8_t m[6];
 if(!boardMac(m))return;
 if(IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane)){
  if(!chosen->getProperty("mac-address-wifi0"))chosen->setProperty("mac-address-wifi0",m,sizeof m);
  chosen->release();
 }
 if(IORegistryEntry *wlan=IORegistryEntry::fromPath("/arm-io/wlan",gIODTPlane)){
  if(!wlan->getProperty("local-mac-address"))wlan->setProperty("local-mac-address",m,sizeof m);
  wlan->release();
 }
}
int pFwGet(const char *path,const uint8_t **data,size_t *size){
 const char *base=path;
 for(const char *c=path;*c;++c)if(*c=='/')base=c+1;
 if(!strcmp(base,"wlan_mac.bin")){
  static char text[40];static size_t len;
  if(!len){uint8_t m[6];if(!boardMac(m)){nwLog(4,"wifi: firmware %s: no unique-chip-id",base);return -1;}
   len=(size_t)snprintf(text,sizeof text,"address=%02x%02x%02x%02x%02x%02x\n",m[0],m[1],m[2],m[3],m[4],m[5]);}
  *data=(const uint8_t *)text;*size=len;return 0;
 }
 const int rc=fuxi_wifi_firmware_get(base,data,size);
 if(rc==-2){nwLog(6,"wifi: firmware %s not provided",path);return -1;}
 if(rc){nwLog(3,"wifi: firmware %s inflate failed: %d",base,rc);return -1;}
 nwLog(6,"wifi: firmware %s %zu bytes (embedded zlib)",base,*size);
 return 0;
}

// ---- configuration space through the iATU window; offsets with bit 31 set are relative to the PCI Express capability ----
uint32_t gPcieCap,gMsiCap;
bool findCaps(){
 uint8_t cap=(uint8_t)(gCfg[0x34/4]&0xff);
 for(unsigned guard=0;cap>=0x40&&guard<48;++guard){
  const uint32_t w=gCfg[cap/4];
  if((uint8_t)w==0x10)gPcieCap=cap;
  if((uint8_t)w==0x05)gMsiCap=cap;
  cap=(uint8_t)(w>>8);
 }
 return gPcieCap&&gMsiCap;
}
int pCfgRead(int off,int size,uint32_t *v){
 uint32_t o=(uint32_t)off;
 if(o&0x80000000u)o=gPcieCap+(o&0x7fffffffu);
 const uint32_t w=gCfg[(o&~3u)/4];
 *v=size==4?w:size==2?(w>>((o&2)*8))&0xffff:(w>>((o&3)*8))&0xff;
 return 0;
}
int pCfgWrite(int off,int size,uint32_t v){
 uint32_t o=(uint32_t)off;
 if(o&0x80000000u)o=gPcieCap+(o&0x7fffffffu);
 if(size==4){gCfg[o/4]=v;return 0;}
 const unsigned shift=(size==2?(o&2):(o&3))*8;
 const uint32_t mask=(size==2?0xffffu:0xffu)<<shift;
 gCfg[(o&~3u)/4]=(gCfg[(o&~3u)/4]&~mask)|((v<<shift)&mask);
 return 0;
}
int pMsiTarget(uint32_t *lo,uint32_t *hi,uint32_t *data){*lo=(uint32_t)gArena.iova;*hi=(uint32_t)(gArena.iova>>32);*data=0;return 0;}

// ---- the endpoint's MSIs through PCIe0's DesignWare iMSI receiver (SPI 141): they only wake the worker, which then steps the module ----
constexpr uint32_t kMsiAddrLo=0x820,kMsiAddrHi=0x824,kMsiEnable0=0x828,kMsiMask0=0x82c,kMsiStatus0=0x830,kMsiSpi=141;
volatile uint32_t gMsiCount,gMsiBits,gWake;
bool gMsiAttached;
IOLock *gWakeLock;
// fuxi 2026-11-20: the worker used to check gWake under gWakeLock and then sleep, while the MSI handler set it and woke the event without
// that lock: an interrupt between the check and the sleep was lost and the worker slept the full 10 ms (en0 measured ~22 Mbit/s at
// ~2000 packets/s on a 1000 Mbps link). Now the worker asserts the wait first and checks the flag after, so a wakeup can no longer fall
// between them; thread_wakeup is safe from the interrupt. gWorkerStats: waits, returns with work already flagged, woken, timed out.
volatile uint32_t gWorkerStats[4];
void wakeWorker(){__atomic_store_n(&gWake,1u,__ATOMIC_RELEASE);thread_wakeup((event_t)&gWake);}
void msiIrq(void *){
 const uint32_t status=gDbi[kMsiStatus0/4];
 if(status)gDbi[kMsiStatus0/4]=status;
 gMsiCount=gMsiCount+1;gMsiBits=gMsiBits|status;
 static uint32_t empty;
 empty=status?0:empty+1;
 if(empty>100000){gDbi[kMsiEnable0/4]=0;gMsiAttached=false;}
 wakeWorker();
}
int msiAttach(){
 if(!armcpu_spi_attach)return -78;
 gDbi[kMsiAddrLo/4]=uint32_t(gArena.iova);
 gDbi[kMsiAddrHi/4]=uint32_t(gArena.iova>>32);
 gDbi[kMsiMask0/4]=0;
 gDbi[kMsiEnable0/4]=0xffffffffu;
 const int rc=armcpu_spi_attach(32+kMsiSpi,false,msiIrq,nullptr,nullptr,nullptr);
 gMsiAttached=rc==0;
 return rc;
}
void workerWait(unsigned ms){
 gWorkerStats[0]=gWorkerStats[0]+1;
 if(__atomic_exchange_n(&gWake,0u,__ATOMIC_ACQ_REL)){gWorkerStats[1]=gWorkerStats[1]+1;return;}
 uint64_t deadline=0;clock_interval_to_deadline(ms,kMillisecondScale,&deadline);
 assert_wait_deadline((event_t)&gWake,THREAD_UNINT,deadline);
 if(__atomic_exchange_n(&gWake,0u,__ATOMIC_ACQ_REL)){clear_wait(current_thread(),THREAD_AWAKENED);gWorkerStats[1]=gWorkerStats[1]+1;return;}
 const wait_result_t r=thread_block(THREAD_CONTINUE_NULL);
 if(r==THREAD_TIMED_OUT)gWorkerStats[3]=gWorkerStats[3]+1;else gWorkerStats[2]=gWorkerStats[2]+1;
 __atomic_store_n(&gWake,0u,__ATOMIC_RELEASE);
}

// ---- the network side: WifiNet's classes are registered with the kernel's module like the other late classes, then en0 is made ----
bool netStart(){
 struct nw_wifi_status status;nw_wifi_status_get(&status);
 void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
 if(!handle){nwLog(3,"wifi: network classes: no module handle");return false;}
 fuxi_wifi_net_static_init();
 const OSReturn r=OSMetaClass::postModLoad(handle);
 if(r){nwLog(3,"wifi: network classes not registered: %d",(int)r);return false;}
 fuxi_wifi_net_start(status.mac);
 return true;
}

// A second thread that reports while the worker may be inside the module (nw_wifi_start runs the MHI power-up and its waits there).
volatile uint32_t gStartDone;
void watchThread(void *,wait_result_t){
 thread_set_thread_name(current_thread(),"fuxi-wifi-watch");
 for(unsigned n=0;;++n){
  IOSleep(n<12?5000:30000);
  nwLog(5,"watch: start-done %u stage %u scans %u bss %u frames %u last_err %d where kind %llu fn %llx since %llu repeats %u MSIs %u DMA %zu/%zu",
        gStartDone,nw_wifi_stats.stage,nw_wifi_stats.scans,nw_wifi_stats.bss_seen,nw_wifi_stats.frames,nw_wifi_stats.last_err,
        nw_wifi_where[0],nw_wifi_where[1],nw_wifi_where[3],gLogRepeats,gMsiCount,gArena.used,gArena.peak);
  nwLog(5,"watch: worker waits %u flagged %u woken %u timeouts %u; packet slab live %u failures %u",gWorkerStats[0],gWorkerStats[1],
        gWorkerStats[2],gWorkerStats[3],gStream.live,gStream.failures);
  if(gStartDone)continue;
  const uint32_t cmd=gCfg[1],id=gCfg[0];
  if(id!=0x110717cbu||!(cmd&2u))continue;
  const uint32_t bhi=gBar[0x28/4];
  nwLog(5,"watch: ep cmd %08x mhi ctrl %08x status %08x ee %08x",cmd,gBar[0x38/4],gBar[0x48/4],bhi>=0x100&&bhi<0x1000&&!(bhi&3)?gBar[(bhi+0x28)/4]:0);
 }
}
}  // namespace

// nw_net.c alloc_skb: a packet buffer from the cached slab, or the write-combining window when that is off or the class is full.
extern "C" void *fuxi_wifi_dma_alloc_stream(size_t n,uint64_t *bus){
 if(gStreamOn)if(void *p=streamAlloc(n,bus))return p;
 return arenaAlloc(n,bus);
}
// The network side asks the worker to send a queued frame now (FuxiWifiNet.cpp if_output) instead of at its next wake.
extern "C" void fuxi_wifi_wake(void){wakeWorker();}
// The address in /chosen at boot, before userland: with /defaults/vmm-present (the iOS 17 ADT) MobileGestalt answers the wifi
// capability from /chosen/mac-address-wifi0 alone, and Settings shows no Wi-Fi row while it is false. Published only once the
// firmware ran, the property came after MobileGestaltHelper had answered (and cached) false. It needs only unique-chip-id.
extern "C" void fuxi_wifi_publish_mac(void){publishMac();}
// Files the network side may look for (sheng: join.txt in the firmware container for unattended joins): none on fuxi.
extern "C" int fuxi_wifi_file(const char *,const uint8_t **,size_t *){return -1;}
extern "C" void native_wifi_logf(int level,const char *fmt,...){
 char line[180];va_list ap;va_start(ap,fmt);vsnprintf(line,sizeof line,fmt,ap);va_end(ap);pLog(level,line);
}

// Called by fuxi_wifi_mhi_start (FuxiWifiMhi.cpp) once the link is up, the BAR assigned, the RC0 SID table reset, the SMMU window mapped and
// bus mastering on. Never returns.
extern "C" void fuxi_wifi_nw_run(volatile uint32_t *bar,size_t barBytes,volatile uint32_t *cfg,volatile uint32_t *dbi,uint8_t *va,uint64_t iova,size_t bytes,
                                 volatile uint32_t *fault,volatile uint64_t *far){
 gCfg=cfg;gDbi=dbi;gBar=bar;gFault=fault;gFar=far;
 PE_parse_boot_argn("fuxi-wifi-log",&gLogLevel,sizeof gLogLevel);
 gWakeLock=IOLockAlloc();
 if(!gWakeLock||!arenaInit(va,iova,streamInit(va,iova,bytes))){nwLog(3,"wifi: arena setup failed");return;}
 if(!findCaps()){nwLog(3,"wifi: PCIe/MSI capability not found");return;}
 publishMac();
 nwLog(5,"wifi: Ath12kWifi f6bf561 (21H461 runtime) start: BAR %zu bytes, DMA window %zu MiB at iova %llx, PCIe cap %x MSI cap %x",barBytes,bytes>>20,iova,gPcieCap,gMsiCap);
 static const struct lx_platform plat={pNow,pSleep,pPoll,pLog,pHalt,hAlloc,hFree,pDmaAlloc,pDmaFree,pDmaMap,pDmaSync,pFwGet};
 static struct nw_board board;
 board.bar=const_cast<uint32_t *>(bar);board.bar_len=barBytes;board.device_id=(uint16_t)(gCfg[0]>>16);board.msi_cap=(uint16_t)gMsiCap;board.irq_base=1000;
 board.cfg_read=pCfgRead;board.cfg_write=pCfgWrite;board.msi_target=pMsiTarget;board.map_phys=nullptr;
 {thread_t w=nullptr;if(kernel_thread_start(watchThread,nullptr,&w)==KERN_SUCCESS)thread_deallocate(w);}
 const int r=nw_wifi_start(&plat,&board);
 gStartDone=1;
 nwLog(r?3:5,"wifi: nw_wifi_start result %d stage %u",r,nw_wifi_stats.stage);
 if(r)for(;;)IOSleep(1000);
 const int msi=msiAttach();
 nwLog(msi?4:5,msi?"wifi: MSI line not attached (%d), polling":"wifi: MSI line attached (SPI 141, result %d), sleeping between interrupts",msi);
 uint32_t lastStage=nw_wifi_stats.stage,lastScans=0;bool netDone=false,faultShown=false;
 for(;;){
  const int busy=nw_wifi_step();
  if(nw_wifi_stats.stage!=lastStage){nwLog(5,"wifi: stage %u -> %u",lastStage,nw_wifi_stats.stage);lastStage=nw_wifi_stats.stage;}
  if(!netDone&&nw_wifi_stats.stage>=4){netDone=true;nwLog(5,"wifi: network side %s",netStart()?"started":"failed");}
  if(nw_wifi_stats.scans!=lastScans){lastScans=nw_wifi_stats.scans;nwLog(5,"wifi: scan %u done: %u networks",lastScans,nw_wifi_stats.bss_seen);}
  if(!faultShown&&gFault&&*gFault){faultShown=true;nwLog(3,"wifi: SMMU fault fsr %x far %llx; endpoint bus master stopped",*gFault,gFar?*gFar:0ull);}
  if(!busy){if(gMsiAttached)workerWait(10);else IOSleep(1);}
 }
}
