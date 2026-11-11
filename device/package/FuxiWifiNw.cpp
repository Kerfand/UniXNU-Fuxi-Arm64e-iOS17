// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fuxi board side of the Ath12kWifi module (fuxi-wifi=5), after UniXNU-sheng WifiProbe.cpp stage 6 (965460a). The module does its own chip
// power-up, MHI boot, QRTR/QMI and driver start; this file gives it register access, configuration space, the PCIe0 MSI line, DMA memory
// from the endpoint's SMMU window (FuxiWifiMhi.cpp dmaPrepare), the embedded (zlib) WCN7850 firmware and a log.
// Fuxi differences: the DMA window is FuxiWifiMhi's own translated aperture (fuxi's NativeUfs exports no native_ufs_extra_* window), the
// firmware is embedded in the kernel (FuxiWifiFirmware.S) instead of read from the Data addon, and no network interface is made yet:
// an in-kernel control client asks cfg80211 for scans and the results go to the log, which is what step A verifies.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <stdarg.h>
#include "nw_abi.h"
#include "nw_control_endpoint.h"
extern "C" int armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *) __attribute__((weak_import));
extern "C" int fuxi_wifi_firmware_get(const char *,const uint8_t **,size_t *);   // FuxiWifiFwStore.cpp
extern "C" void _disable_preemption(void);
extern "C" void _enable_preemption(void);
extern "C" uint64_t mach_continuous_time(void);
namespace {
volatile uint32_t *gCfg,*gDbi;
volatile uint32_t *gFault;volatile uint64_t *gFar;
volatile uint32_t *gBar;
// 7 while the module's power-up is brought up on this board (its debug lines, capped in pLog); fuxi-wifi-log=6 for notices only
unsigned gLogLevel=7;

// ---- log: one IOLog line per message on the USB console; a line equal to one of the last sixteen is only counted ----
uint32_t gLogRepeats;
bool has(const char *s,const char *w){for(;*s;++s){size_t k=0;while(w[k]&&s[k]==w[k])++k;if(!w[k])return true;}return false;}
uint64_t pNowNs(){uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns;}
uint64_t pBootNs(){uint64_t ns=0;absolutetime_to_nanoseconds(mach_continuous_time(),&ns);return ns;}
uint64_t pNow(){return pNowNs()/1000000;}
uint32_t gDebugLines;
void pLog(int level,const char *line){
 if(level>(int)gLogLevel)return;
 if(level>=7&&++gDebugLines>4000)return;   // the driver's debug lines: enough for the bring-up, never a console flood
 // the per-channel and per-buffer lines sheng filters out of its ring (they would flood the console)
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

// ---- DMA: carved out of the SMMU window; first page is the MSI target. Buffers are aligned to their size rounded up to a power of two
// (sheng: the boot loader refuses an image address that is only page aligned). The window is mapped uncached, so sync is a barrier. ----
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
 uint32_t freeTotal=0,freeMax=0;
 for(unsigned i=0;i<gArena.count;++i)if(!gArena.blocks[i].used){freeTotal+=gArena.blocks[i].size;if(gArena.blocks[i].size>freeMax)freeMax=gArena.blocks[i].size;}
 nwLog(3,"dma window full: want %zu free %u largest %u blocks %u",n,freeTotal,freeMax,gArena.count);
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
void *pDmaAlloc(size_t n,uint64_t *bus){return arenaAlloc(n,bus);}
void pDmaFree(size_t,void *p,uint64_t){arenaFree(p);}
uint64_t pDmaMap(void *p,size_t,int){
 if((uint8_t *)p<gArena.va||(uint8_t *)p>=gArena.va+gArena.bytes)return 0;
 return gArena.iova+((uint8_t *)p-gArena.va);
}
void pDmaSync(uint64_t,size_t,int){__asm__ volatile("dsb sy":::"memory");}

void pBusyDelayUs(unsigned int us){IODelay(us);}
void pSleep(unsigned ms){IOSleep(ms);}
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

// ---- firmware: the embedded files (wifi-firmware/manifest.json); wlan_mac.bin is made from /chosen/unique-chip-id as on sheng
// (this firmware reports a zero MAC) ----
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

// ---- the endpoint's MSIs: the DesignWare iMSI receiver of PCIe0 takes the inbound write to MSI_ADDR (the arena's first page) and raises
// SPI 141 (sm8550.dtsi pcie0 msi0), as on sheng. The handler clears the status and wakes the worker. ----
constexpr uint32_t kMsiAddrLo=0x820,kMsiAddrHi=0x824,kMsiEnable0=0x828,kMsiMask0=0x82c,kMsiStatus0=0x830,kMsiSpi=141;
volatile uint32_t gMsiPending,gMsiCount,gMsiBits,gWake;
bool gMsiAttached;
IOLock *gWakeLock;
void wakeWorker(){__atomic_store_n(&gWake,1u,__ATOMIC_RELEASE);if(gWakeLock)IOLockWakeup(gWakeLock,(void *)&gWake,false);}
void msiIrq(void *){
 const uint32_t status=gDbi[kMsiStatus0/4];
 if(status)gDbi[kMsiStatus0/4]=status;
 gMsiCount=gMsiCount+1;gMsiBits=gMsiBits|status;
 __atomic_fetch_or(&gMsiPending,status,__ATOMIC_RELEASE);
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
// Sleeps until an MSI, a control wake or ms milliseconds pass; a wake that came since the last wait returns at once.
void workerWait(unsigned ms){
 if(__atomic_exchange_n(&gWake,0u,__ATOMIC_ACQ_REL))return;
 uint64_t deadline=0;clock_interval_to_deadline(ms,kMillisecondScale,&deadline);
 IOLockLock(gWakeLock);
 if(!__atomic_load_n(&gWake,__ATOMIC_ACQUIRE))IOLockSleepDeadline(gWakeLock,(void *)&gWake,*reinterpret_cast<AbsoluteTime *>(&deadline),THREAD_UNINT);
 IOLockUnlock(gWakeLock);
 __atomic_store_n(&gWake,0u,__ATOMIC_RELEASE);
}
void pPoll(){
 nw_wifi_irq_mode(gMsiAttached);
 const uint32_t pending=__atomic_exchange_n(&gMsiPending,0u,__ATOMIC_ACQ_REL);
 if(pending)nw_wifi_irq_pending(pending);
 nw_wifi_poll();
}

// ---- events: no network interface yet (step A); only the scan result is used ----
volatile uint32_t gScanDone;
void evScanDone(){__atomic_store_n(&gScanDone,1u,__ATOMIC_RELEASE);}
void evConnectDone(int){}
void evLinkLost(int){}
void evRx(const uint8_t *,size_t){}
void evRxFlush(){}
void evRxPacket(void *,size_t,nw_packet_reader){}
void evLinkUpdate(const struct nw_wifi_status *){}

// ---- an in-kernel control client in place of the supplicant: INFO, then a wildcard SCAN (nw_control.h wire format) ----
// Every request but INFO carries the generation of the current attempt (nw_cfg80211_control.c: others are refused NW_CONTROL_STALE);
// replies and events bring it in their header, and it changes with the radio state, so the latest one seen is used.
uint64_t gSession,gGeneration;uint32_t gSeq;
void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
void put32(uint8_t *p,uint32_t v){put16(p,(uint16_t)v);put16(p+2,(uint16_t)(v>>16));}
void put64(uint8_t *p,uint64_t v){put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32));}
uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int ctlSend(void *,uint64_t,const void *bytes,size_t length){
 const uint8_t *p=static_cast<const uint8_t *>(bytes);
 if(length>=NW_CONTROL_HEADER_SIZE){
  const unsigned kind=p[6]|p[7]<<8,op=p[8]|p[9]<<8;
  __atomic_store_n(&gGeneration,(uint64_t)get32(p+32)|(uint64_t)get32(p+36)<<32,__ATOMIC_RELEASE);
  if(kind==NW_CONTROL_REPLY)nwLog(6,"wifi: control reply op %u status %d generation %llu (%zu bytes)",op,(int)get32(p+20),gGeneration,length);
  else if(kind==NW_CONTROL_EVENT&&op!=NW_CONTROL_WIRELESS_EVENT)nwLog(6,"wifi: control event %#x (%zu bytes)",op,length);
 }
 return NW_CONTROL_SEND_OK;
}
void ctlWake(void *){wakeWorker();}
int readFlat(void *context,void *destination,size_t length){memcpy(destination,context,length);return 0;}
int request(unsigned op,unsigned attribute,const void *value,size_t valueLength){
 uint8_t b[128]={};
 size_t length=NW_CONTROL_HEADER_SIZE;
 if(attribute){
  const size_t padded=(valueLength+7u)&~(size_t)3u;
  if(length+padded>sizeof b)return NW_CONTROL_INVALID;
  put16(b+length,(uint16_t)attribute);put16(b+length+2,(uint16_t)(valueLength+4));memcpy(b+length+4,value,valueLength);
  length+=padded;
 }
 put32(b,NW_CONTROL_MAGIC);put16(b+4,NW_CONTROL_VERSION);put16(b+6,NW_CONTROL_REQUEST);put16(b+8,(uint16_t)op);put16(b+10,0);
 put32(b+12,++gSeq);put32(b+16,(uint32_t)length);put32(b+20,0);put64(b+24,gSession);put64(b+32,op==NW_CONTROL_INFO?0:__atomic_load_n(&gGeneration,__ATOMIC_ACQUIRE));
 return nw_wifi_control_receive(gSession,length,readFlat,b);
}
int scanRequest(){
 uint8_t wildcard[33]={};   // one SSID record: length 0 (any network), 32 padded bytes; an active scan as the supplicant asks for it
 return request(NW_CONTROL_SCAN,NW_CONTROL_ATTR_SCAN_SSIDS,wildcard,sizeof wildcard);
}
void logScan(){
 const int count=nw_wifi_bss_count();
 nwLog(5,"wifi: scan %u done: %d networks",nw_wifi_stats.scans,count);
 struct nw_bss_info info;
 for(int i=0;i<count&&i<24;++i){
  if(nw_wifi_bss_get(i,&info))break;
  char ssid[33];const unsigned n=info.ssid_len<32?info.ssid_len:32;
  for(unsigned k=0;k<n;++k)ssid[k]=(info.ssid[k]>=0x20&&info.ssid[k]<0x7f)?(char)info.ssid[k]:'?';ssid[n]=0;
  nwLog(5,"wifi: bss %d %02x:%02x:%02x:..:%02x %u MHz %d dBm \"%s\"",i,info.bssid[0],info.bssid[1],info.bssid[2],info.bssid[5],info.freq_mhz,info.signal_dbm,ssid);
 }
}

// A second thread that reports while the worker may be inside the module (nw_wifi_start runs the MHI power-up and its 90 s waits there):
// first what the module says it runs (nw_wifi_where: kind, function as an offset from lx_now_ms, depth, start ms) from memory only, then,
// in a line of its own (a register read of a stalled endpoint could stall this core), MHI and PCIe state.
volatile uint32_t gStartDone;
void watchThread(void *,wait_result_t){
 thread_set_thread_name(current_thread(),"fuxi-wifi-watch");
 for(unsigned n=0;;++n){
  IOSleep(n<36?5000:30000);
  nwLog(5,"watch: start-done %u stage %u last_err %d where kind %llu fn %llx depth %llu since %llu repeats %u debug %u MSIs %u",
        gStartDone,nw_wifi_stats.stage,nw_wifi_stats.last_err,nw_wifi_where[0],nw_wifi_where[1],nw_wifi_where[2],nw_wifi_where[3],gLogRepeats,gDebugLines,gMsiCount);
  const uint32_t cmd=gCfg[1],id=gCfg[0],link=gDbi[0x80/4],ltssm=gDbi[0x728/4];
  nwLog(5,"watch: ep id %08x cmd %08x root link %08x ltssm %08x",id,cmd,link,ltssm);
  if(id!=0x110717cbu||!(cmd&2u))continue;   // the BAR is only read while the endpoint answers with memory decoding on
  const uint32_t bhi=gBar[0x28/4],bhie=gBar[0x2c/4];
  const bool ok=bhi>=0x100&&bhi<0x1000&&!(bhi&3)&&bhie>=0x100&&bhie<0x1000&&!(bhie&3);
  nwLog(5,"watch: mhi ctrl %08x status %08x bhi %x bhie %x ee %08x bhi-status %08x err %08x bhie-tx %08x",gBar[0x38/4],gBar[0x48/4],bhi,bhie,
        ok?gBar[(bhi+0x28)/4]:0,ok?gBar[(bhi+0x2c)/4]:0,ok?gBar[(bhi+0x30)/4]:0,ok?gBar[(bhie+0x44)/4]:0);
 }
}
void report(){
 nwLog(5,"wifi: stage %u scans %u bss %u frames %u beacons %u last_err %d",nw_wifi_stats.stage,nw_wifi_stats.scans,nw_wifi_stats.bss_seen,nw_wifi_stats.frames,nw_wifi_stats.beacons,nw_wifi_stats.last_err);
 nwLog(5,"wifi: MSIs %u vectors %08x attached %u; DMA used %zu peak %zu of %zu failures %u",gMsiCount,gMsiBits,gMsiAttached?1u:0u,gArena.used,gArena.peak,gArena.bytes,gArena.failures);
 nwLog(6,"wifi: where kind %llu fn %llx depth %llu since %llu; log repeats %u",nw_wifi_where[0],nw_wifi_where[1],nw_wifi_where[2],nw_wifi_where[3],gLogRepeats);
}
}  // namespace

// Called by fuxi_wifi_mhi_start (FuxiWifiMhi.cpp) once the link is up, the BAR assigned, the RC0 SID table reset and the SMMU window mapped.
// Never returns: the module keeps DMA, the fault handler and the PCIe windows for the rest of the boot.
extern "C" void fuxi_wifi_nw_run(volatile uint32_t *bar,size_t barBytes,volatile uint32_t *cfg,volatile uint32_t *dbi,uint8_t *va,uint64_t iova,size_t bytes,
                                 volatile uint32_t *fault,volatile uint64_t *far){
 gCfg=cfg;gDbi=dbi;gBar=bar;gFault=fault;gFar=far;
 PE_parse_boot_argn("fuxi-wifi-log",&gLogLevel,sizeof gLogLevel);
 gWakeLock=IOLockAlloc();
 if(!gWakeLock||!arenaInit(va,iova,bytes)){nwLog(3,"wifi: arena setup failed");return;}
 if(!findCaps()){nwLog(3,"wifi: PCIe/MSI capability not found");return;}
 nwLog(5,"wifi: Ath12kWifi start: BAR %zu bytes, DMA window %zu MiB at iova %llx, PCIe cap %x MSI cap %x",barBytes,bytes>>20,iova,gPcieCap,gMsiCap);
 static const struct lx_platform plat={pNow,pSleep,pPoll,pLog,pHalt,hAlloc,hFree,pDmaAlloc,pDmaFree,pDmaMap,pDmaSync,pFwGet,pNowNs,pBootNs,pBusyDelayUs,_disable_preemption,_enable_preemption};
 nw_wifi_set_platform(&plat);
 static const struct nw_wifi_events events={evScanDone,evConnectDone,evLinkLost,evRx,evRxFlush,evRxPacket,evLinkUpdate};
 nw_wifi_set_events(&events);
 static struct nw_board board;
 board.bar=const_cast<uint32_t *>(bar);board.bar_len=barBytes;board.device_id=(uint16_t)(gCfg[0]>>16);board.msi_cap=(uint16_t)gMsiCap;board.irq_base=1000;
 board.cfg_read=pCfgRead;board.cfg_write=pCfgWrite;board.msi_target=pMsiTarget;board.map_phys=nullptr;
 static const nw_control_transport transport={nullptr,ctlSend,ctlWake};
 const int transportRc=nw_wifi_control_transport_set(&transport);
 if(transportRc)nwLog(4,"wifi: control transport result %d",transportRc);
 {thread_t w=nullptr;if(kernel_thread_start(watchThread,nullptr,&w)==KERN_SUCCESS)thread_deallocate(w);}
 const int r=nw_wifi_start(&plat,&board);
 gStartDone=1;
 nwLog(r?3:5,"wifi: nw_wifi_start result %d stage %u",r,nw_wifi_stats.stage);
 if(r){report();for(;;)IOSleep(1000);}
 const int msi=msiAttach();
 nw_wifi_irq_mode(gMsiAttached);
 nwLog(msi?4:5,msi?"wifi: MSI line not attached (%d), polling":"wifi: MSI line attached (SPI 141, result %d), sleeping between interrupts",msi);
 uint32_t lastStage=nw_wifi_stats.stage,scans=0;bool faultShown=false;
 uint64_t nextReport=pNow()+5000,nextScan=0,attachUntil=0,sliceStarted=pNowNs();
 for(;;){
  const int busy=nw_wifi_step();
  const uint64_t now=pNow();
  if(nw_wifi_stats.stage!=lastStage){nwLog(5,"wifi: stage %u -> %u",lastStage,nw_wifi_stats.stage);lastStage=nw_wifi_stats.stage;}
  if(!faultShown&&gFault&&*gFault){faultShown=true;nwLog(3,"wifi: SMMU fault fsr %x far %llx; endpoint bus master stopped",*gFault,gFar?*gFar:0ull);}
  // once the interface is up: attach the control client (it binds when cfg80211 is ready), then scan at once and every 60 s, five times
  if(nw_wifi_stats.stage>=4&&!gSession&&now>=attachUntil){
   const int a=nw_wifi_control_attach(&gSession);
   if(a){gSession=0;attachUntil=now+1000;nwLog(6,"wifi: control attach result %d",a);}
   else{nwLog(5,"wifi: control session %llu",gSession);request(NW_CONTROL_INFO,0,nullptr,0);nextScan=now+2000;}
  }
  if(gSession&&scans<5&&nextScan&&now>=nextScan){const int s=scanRequest();nwLog(5,"wifi: scan request %u result %d",scans+1,s);++scans;nextScan=now+60000;}
  if(__atomic_exchange_n(&gScanDone,0u,__ATOMIC_ACQ_REL))logScan();
  if(now>=nextReport){nextReport=now+30000;report();}
  if(!busy){
   if(gMsiAttached){const unsigned wait=nw_wifi_wait_ms(1000);if(wait)workerWait(wait<10?wait:10);}
   else IOSleep(1);
   sliceStarted=pNowNs();
  }else if(pNowNs()-sliceStarted>=2000000){IOSleep(1);sliceStarted=pNowNs();}
 }
}
