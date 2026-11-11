// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fuxi WCN7850 mission-mode boot. No station or OS network interface yet.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "ArmSmmu4K.h"
#include "ArmSmmu4KContext.h"
#include "ArmSmmu4KProgram.h"
#include "ArmSmmu4KStreams.h"
#include "MhiBoot.h"
#include "FuxiWifiSid.h"
extern "C" int armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *) __attribute__((weak_import));
extern "C" int fuxi_wifi_firmware_get(const char *,const uint8_t **,size_t *);   // FuxiWifiNw.cpp: the embedded zlib copy, inflated once
extern "C" void fuxi_wifi_nw_run(volatile uint32_t *,size_t,volatile uint32_t *,volatile uint32_t *,uint8_t *,uint64_t,size_t,volatile uint32_t *,volatile uint64_t *);
namespace {
volatile uint32_t native_wifi_chip[16];
volatile uint32_t *gCfg,*gDbi;

struct Dma{uint8_t *va;uint64_t pa;size_t bytes;};
// fuxi-wifi=3 (MhiBoot below): 16 MiB, one contiguous cached block. fuxi-wifi=5 (Ath12kWifi, FuxiWifiNw.cpp): sheng's 128 MiB, uncached pages
// that need not be contiguous, mapped run by run. 72 table pages cover 128 MiB of 4K leaves (64 level-3 tables and the levels above).
constexpr unsigned kTablePages=72,kModuleDmaBytes=128u<<20;
constexpr uint64_t kIova=0x80000000ull;
unsigned gDmaBytes=16u<<20;bool gScattered;
struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}


void put64(volatile uint32_t *regs, uint32_t lowOffset, uint64_t value) {regs[lowOffset / 4] = (uint32_t)value; regs[(lowOffset + 4) / 4] = (uint32_t)(value >> 32);}

// The device context arrays and rings (mhi_init_dev_ctxt): 128 channel contexts left disabled, two event rings (control 32 elements, data 256) and the
// primary command ring, in one buffer; then the registers that point the device at them (mhi_init_mmio).
uint8_t *gEvVa[2]; uint8_t *gCtxVa;
bool programContexts(volatile uint32_t *m, Dma &c, uint64_t mapped) {
    const uint32_t erdb = m[0x20 / 4];
    if(erdb<0x100 || (erdb&7u) || mapped<16 || erdb>mapped-16) {
      IOLog("[FuxiWifi] context doorbell layout refused erdb=%x mapped=%llx\n",erdb,mapped);return false;
    }
    constexpr uint32_t kCtx = 44, kChannels = 128, kEvents = 2, kCmdEl = 8, kEl = 16;
    const uint32_t evLen[kEvents] = {32 * kEl, 256 * kEl};
    uint8_t *va = c.va;
    gCtxVa = va;
    const uint64_t pa = c.pa;
    const uint32_t chOff = 0, evOff = kChannels * kCtx, cmdOff = evOff + kEvents * kCtx;
    uint32_t ringOff = (cmdOff + kCtx + 4095) & ~4095u;
    uint32_t cur = ringOff;
    auto w32 = [&](uint32_t at, uint32_t v) {memcpy(va + at, &v, 4);};
    auto w64 = [&](uint32_t at, uint64_t v) {memcpy(va + at, &v, 8);};
    uint64_t evBase[kEvents];
    for (uint32_t i = 0; i < kEvents; ++i) {
        evBase[i] = pa + cur;
        gEvVa[i] = va + cur;
        const uint32_t at = evOff + i * kCtx;
        w32(at, i == 0 ? 0 : 1u << 16);          // interrupt moderation 0 / 1 ms
        w32(at + 4, 1);                          // MHI_ER_TYPE_VALID
        w32(at + 8, i + 1);                      // MSI vector
        w64(at + 12, evBase[i]);
        w64(at + 20, evLen[i]);
        w64(at + 28, evBase[i]);                 // read pointer
        w64(at + 36, evBase[i] + evLen[i] - kEl);   // write pointer: the whole ring is free for the device
        cur += evLen[i];
    }
    const uint64_t cmdBase = pa + cur;
    w64(cmdOff + 12, cmdBase);
    w64(cmdOff + 20, kCmdEl * kEl);
    w64(cmdOff + 28, cmdBase);
    w64(cmdOff + 36, cmdBase);
    __asm__ volatile("dsb sy" ::: "memory");
    put64(m, 0x58, pa + chOff);                  // CCABAP
    put64(m, 0x60, pa + evOff);                  // ECABAP
    put64(m, 0x68, pa + cmdOff);                 // CRCBAP
    put64(m, 0x80, 0); put64(m, 0x88, 0xffffffffull);   // control base and limit
    put64(m, 0x98, 0); put64(m, 0xa0, 0xffffffffull);   // data base and limit
    m[0x10 / 4] = (m[0x10 / 4] & ~0xff000000u & ~0x00ff0000u) | (kEvents << 16);   // MHICFG: NER = 2, NHWER = 0
    // event ring doorbells (ERDBOFF + 8 * ring): the write pointers
    if (erdb <= mapped - 8 * kEvents) for (uint32_t i = 0; i < kEvents; ++i) put64(m, erdb + 8 * i, evBase[i] + evLen[i] - kEl);
    __asm__ volatile("dsb sy" ::: "memory");
    return true;
}

uint32_t gWindow = 0xffffffffu;
uint32_t chipRead(volatile uint32_t *m, uint32_t off) {
    if (off < 0x80000) return m[off / 4];
    const uint32_t window = (off >> 19) & 0x3fu;
    if (window != gWindow) {m[0x310c / 4] = 0x40000000u | window; (void)m[0x310c / 4]; gWindow = window;}
    if (off >= 0x1e0e100u && off <= 0x1e0effcu) return m[((off - 0x1e0e100u) & 0x7ffffu) / 4];
    return m[(0x80000u + (off & 0x7ffffu)) / 4];
}
void chipWrite(volatile uint32_t *m, uint32_t off, uint32_t value) {
    if (off < 0x80000) {m[off / 4] = value; __asm__ volatile("dsb sy" ::: "memory"); return;}
    const uint32_t window = (off >> 19) & 0x3fu;
    if (window != gWindow) {m[0x310c / 4] = 0x40000000u | window; (void)m[0x310c / 4]; gWindow = window;}
    if (off >= 0x1e0e100u && off <= 0x1e0effcu) m[((off - 0x1e0e100u) & 0x7ffffu) / 4] = value;
    else m[(0x80000u + (off & 0x7ffffu)) / 4] = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
// ath12k_pci_sw_reset(power on). Values seen along the way go to native_wifi_chip: [0] LTSSM word, [1] hot reset word, [2] Q6 cookie, [3] warm entry,
// [4] warm entry after clearing, [5] reset cause, [6] global reset word before, [7] after, [8] MHISTATUS before the MHI reset, [9] MHISTATUS after, [10] EE after.
void chipPowerUp(volatile uint32_t *m) {
    volatile uint32_t *c = native_wifi_chip;
    gWindow = 0xffffffffu;
    uint32_t v = chipRead(m, 0x1e081b0u);
    for (unsigned i = 0; v != 0x111u && i < 5; ++i) {
        if (v == 0xffffffffu) IOSleep(5);
        chipWrite(m, 0x1e081b0u, 0x111u);
        v = chipRead(m, 0x1e081b0u);
    }
    c[0] = v;
    v = chipRead(m, 0x1e40304u); v |= 0x10u; chipWrite(m, 0x1e40304u, v); c[1] = chipRead(m, 0x1e40304u);
    IOSleep(5);
    chipWrite(m, 0x1e08228u, 0xffffffffu);   // interrupts left over from a hot reset
    v = chipRead(m, 0x1f8031cu); v &= ~0x4u; chipWrite(m, 0x1f8031cu, v);
    chipWrite(m, 0x360, 0); chipWrite(m, 0x368, 0); chipWrite(m, 0x394, 0); chipWrite(m, 0x39c, 0);   // BHIe vector doorbells and status
    c[2] = chipRead(m, 0x1f80500u);
    c[3] = chipRead(m, 0x1f80504u);
    IOSleep(10);
    chipWrite(m, 0x1f80504u, 0);             // keeps the Q6 off the warm boot path
    IOSleep(10);
    c[4] = chipRead(m, 0x1f80504u);
    c[5] = chipRead(m, 0x1f8060cu);          // read-clear
    v = chipRead(m, 0x3008u); c[6] = v;
    chipWrite(m, 0x3008u, v | 1u); (void)chipRead(m, 0x3008u);
    IOSleep(10);
    chipWrite(m, 0x3008u, v & ~1u); (void)chipRead(m, 0x3008u);
    IOSleep(10);
    c[7] = chipRead(m, 0x3008u);
    m[0x310c / 4] = 0x40000000u | (gWindow == 0xffffffffu ? 0u : gWindow);   // the window register is cleared by the reset; put the cached window back
    (void)m[0x310c / 4];
    c[8] = m[0x48 / 4];
    m[0x38 / 4] = 2;                         // MHICTRL reset: clears a SYSERR left by the global reset
    __asm__ volatile("dsb sy" ::: "memory");
    IOSleep(10);
    c[9] = m[0x48 / 4];
    const uint32_t bhi=m[0x28/4];
    c[10] = bhi>=0x100 && !(bhi&3) && bhi<=0x200000-0x2c ? m[(bhi+0x28)/4] : 0xffffffffu;
}
// Without ASPM during the load, and MSI enabled (the endpoint raises its completions as MSI writes; they land in a sink in the DMA buffer, polled later).
void endpointLinkSetup(uint64_t sinkAddress) {
    uint8_t cap = (uint8_t)(gCfg[0x34 / 4] & 0xff);
    for (unsigned guard = 0; cap >= 0x40 && guard < 48; ++guard) {
        const uint32_t w = gCfg[cap / 4];
        const uint8_t id = (uint8_t)w, next = (uint8_t)(w >> 8);
        if (id == 0x10) {   // PCI Express: link control, ASPM bits
            const uint32_t lc = gCfg[(cap + 0x10) / 4];
            native_wifi_chip[11] = lc;
            gCfg[(cap + 0x10) / 4] = lc & ~3u;
        } else if (id == 0x05) {   // MSI: address, data, then 4 vectors and enable
            const bool is64 = (w >> 16) & 0x80;
            gCfg[(cap + 4) / 4] = (uint32_t)sinkAddress;
            if (is64) {gCfg[(cap + 8) / 4] = (uint32_t)(sinkAddress >> 32); gCfg[(cap + 0xc) / 4] = 0;}
            else gCfg[(cap + 8) / 4] = 0;
            gCfg[cap / 4] = (w & 0xffffu) | (((w >> 16) & ~0x70u) | 0x20u | 1u) << 16;
            native_wifi_chip[12] = gCfg[cap / 4];
        }
        cap = next;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}


unsigned eventAck(volatile uint32_t *m,unsigned mapped) {
    constexpr uint32_t kCtx = 44, kChannels = 128;
    unsigned moved = 0;
    const uint32_t erdb = m[0x20 / 4];
    if(erdb<0x100 || (erdb&7u) || mapped<16 || erdb>mapped-16)return 0;
    static uint64_t acked[2];
    for (unsigned r = 0; r < 2; ++r) {
        uint8_t *ctx = gCtxVa + kChannels * kCtx + r * kCtx;
        uint64_t base, len, rp;
        memcpy(&base, ctx + 12, 8); memcpy(&len, ctx + 20, 8); memcpy(&rp, ctx + 28, 8);
        if (!len || len>4096 || (len&15) || (rp&15) || rp == acked[r] || rp < base || rp-base >= len) continue;
        acked[r] = rp;
        const uint64_t wp = rp == base ? base + len - 16 : rp - 16;
        memcpy(ctx + 36, &wp, 8);
        __asm__ volatile("dc civac, %0\n dsb sy" :: "r"(ctx + 36) : "memory");
        m[(erdb + 8 * r) / 4] = (uint32_t)wp; m[(erdb + 8 * r + 4) / 4] = (uint32_t)(wp >> 32);
        ++moved;
    }
    return moved;
}


constexpr unsigned kCbSpi[]={97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,181,182,183,184,185,186,187,188,189,190,191,192,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,337,338,339,340,341,342,343,344,345,395,396,397,398,399,400,401,402,403,404,405,406,407,408,409,418,419,412,421,706,423,424,425,689,690,691,692,693,694,695,696};

struct Session {
 IOBufferMemoryDescriptor *data,*tableMemory;Window smmu,bar;
 uint8_t *va;uint64_t phys,tablePhys;unsigned bank,slot;uint64_t cb;
 ArmSmmu4KConfig config;ArmSmmu4KResult tables;ArmSmmu4KContextConfig context;
 ArmSmmu4KStreamState streams;ArmSmmu4KIo io;volatile uint32_t fault;volatile uint64_t far;
} g;
void clean(void *p,size_t bytes){uintptr_t at=(uintptr_t)p&~63ull,end=(uintptr_t)p+bytes;
 for(;at<end;at+=64)__asm__ volatile("dc civac, %0"::"r"(at):"memory");__asm__ volatile("dsb sy":::"memory");}
int rd32(void *p,size_t o,uint32_t *v){if((o&3)||o+4>0x100000)return -1;*v=((volatile uint32_t *)p)[o/4];return 0;}
int rd64(void *p,size_t o,uint64_t *v){if((o&7)||o+8>0x100000)return -1;*v=((volatile uint64_t *)p)[o/8];return 0;}
int wr32(void *p,size_t o,uint32_t v){if((o&3)||o+4>0x100000)return -1;((volatile uint32_t *)p)[o/4]=v;__asm__ volatile("dsb sy":::"memory");return 0;}
int wr64(void *p,size_t o,uint64_t v){if((o&7)||o+8>0x100000)return -1;((volatile uint64_t *)p)[o/8]=v;__asm__ volatile("dsb sy":::"memory");return 0;}
int barrier(void *){__asm__ volatile("dsb sy":::"memory");return 0;}
void faultIrq(void *){uint32_t fsr=g.smmu.va[(g.cb+0x58)/4];if(!fsr)return;
 g.fault=fsr;g.far=((volatile uint64_t *)g.smmu.va)[(g.cb+0x60)/8];
 if(gCfg)gCfg[1]=gCfg[1]&~4u;g.smmu.va[(g.cb+0x58)/4]=fsr;__asm__ volatile("dsb sy":::"memory");}
bool allocate(IOBufferMemoryDescriptor *&m,size_t bytes,uint64_t &pa,uint8_t *&va){
 m=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,(IOOptionBits)kIODirectionInOut|(IOOptionBits)kIOMemoryPhysicallyContiguous,bytes,0xfffffff000ull);
 if(!m || m->prepare()!=kIOReturnSuccess)return false;
 addr64_t length=0;pa=m->getPhysicalSegment(0,&length);va=(uint8_t *)m->getBytesNoCopy();
 return pa && !(pa&4095) && length>=bytes && va;
}
// The module's window, as UniXNU-ShengUfs (af14068) makes its native_ufs_extra_* window: a cached buffer, zeroed and cleaned through its own
// mapping, then reached by the CPU only through a second, write-combining (normal non-cacheable) mapping, so the endpoint and the CPU see
// the same memory whether or not the link is coherent. Never kIOMapInhibitCache on the buffer itself: that is device memory (memset's
// DC ZVA and unaligned copies fault) and, on RAM above 4 GiB, the cache-attribute path panics in phystokv_range (sheng TouchProbe.cpp).
// One contiguous block is tried first (sheng's layout); otherwise pages, each run of physically consecutive ones one range at
// consecutive IOVAs. Reserved at probe time (fuxi_wifi_dma_reserve), long before the link thread's 65 s wait ends.
ArmSmmu4KRange *gRanges;size_t gRangeCount;
IOBufferMemoryDescriptor *gReserved;IOMemoryMap *gReservedWc;uint8_t *gReservedVa;uint64_t gReservedPa;bool gReservedContiguous;
bool reserveModuleWindow(size_t bytes){
 if(gReservedVa)return true;
 constexpr size_t kBlock=2u<<20;
 IOBufferMemoryDescriptor *m=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,(IOOptionBits)kIODirectionInOut|(IOOptionBits)kIOMemoryPhysicallyContiguous|(IOOptionBits)kIOMemoryMapperNone,bytes+kBlock,kBlock);
 bool contiguous=m!=nullptr;
 if(!m)m=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,(IOOptionBits)kIODirectionInOut|(IOOptionBits)kIOMemoryMapperNone,bytes,4096);
 if(!m || m->prepare()!=kIOReturnSuccess){if(m)m->release();return false;}
 uint8_t *cached=(uint8_t *)m->getBytesNoCopy();
 const size_t total=contiguous?bytes+kBlock:bytes;
 const size_t cap=bytes/4096;
 if(!gRanges)gRanges=(ArmSmmu4KRange *)IOMallocData(cap*sizeof(ArmSmmu4KRange));
 if(!cached||!gRanges){m->complete();m->release();return false;}
 size_t shift=0;
 gRangeCount=0;
 if(contiguous){
  const uint64_t pa=m->getPhysicalSegment(0,nullptr,kIOMemoryMapperNone);
  shift=(kBlock-(pa&(kBlock-1)))&(kBlock-1);
  gRanges[0]={kIova,pa+shift,bytes,ArmSmmu4KRead|ArmSmmu4KWrite,1,3};gRangeCount=1;
 }else{
  for(size_t off=0;off<bytes;){
   IOByteCount length=0;const addr64_t pa=m->getPhysicalSegment(off,&length,kIOMemoryMapperNone);
   if(length>bytes-off)length=bytes-off;
   if(!pa||(pa&4095)||!length||(length&4095)){m->complete();m->release();return false;}
   if(gRangeCount && gRanges[gRangeCount-1].physical+gRanges[gRangeCount-1].bytes==pa)gRanges[gRangeCount-1].bytes+=length;
   else{if(gRangeCount==cap){m->complete();m->release();return false;}gRanges[gRangeCount++]={kIova+off,pa,length,ArmSmmu4KRead|ArmSmmu4KWrite,1,3};}
   off+=length;
  }
 }
 bzero(cached,total);
 for(size_t off=0;off<total;off+=64)__asm__ volatile("dc civac, %0"::"r"(cached+off):"memory");
 __asm__ volatile("dsb sy":::"memory");
 IOMemoryMap *wc=m->createMappingInTask(kernel_task,0,kIOMapAnywhere|kIOMapWriteCombineCache);
 if(!wc){m->complete();m->release();return false;}
 gReserved=m;gReservedWc=wc;gReservedVa=(uint8_t *)wc->getVirtualAddress()+shift;gReservedPa=gRanges[0].physical;gReservedContiguous=contiguous;
 return true;
}
bool dmaPrepare(){
 uint8_t *pool=nullptr;
 bool data;
 if(gScattered){data=reserveModuleWindow(gDmaBytes);if(data){g.data=gReserved;g.va=gReservedVa;g.phys=gReservedPa;}}
 else data=allocate(g.data,gDmaBytes,g.phys,g.va);
 if(!data||!allocate(g.tableMemory,kTablePages*4096,g.tablePhys,pool)||!mapWindow(g.smmu,0x15000000,0x100000)){
  IOLog("[FuxiWifi] MHI DMA allocation/map failed bytes=%u scattered=%u\n",gDmaBytes,gScattered?1u:0u);return false;}
 volatile uint32_t *s=g.smmu.va;uint32_t id0=s[0x20/4],id1=s[0x24/4],id2=s[0x28/4];unsigned slots=id0&255,banks=id1&255,pages=1u<<(((id1>>28)&7)+1);
 if(!slots||slots>128||banks<2||banks>128||pages>128||((id1>>31)&1)){IOLog("[FuxiWifi] SMMU geometry refused id=%x/%x/%x\n",id0,id1,id2);return false;}
 bool used[128]={};int slot=-1;unsigned exact=0;
 for(unsigned i=0;i<slots;++i){uint32_t smr=s[(0x800+4*i)/4],s2=s[(0xc00+4*i)/4];bool valid=(id0&256)?(s2&1024):(smr&0x80000000u);
  if(!valid){if(slot<0)slot=(int)i;continue;}
  unsigned mask=(smr>>16)&((id0&256)?0xffff:0x7fff);
  if(!((smr^0x1401u)&0xffff&~mask)){if(mask || ++exact>1){IOLog("[FuxiWifi] conflicting WLAN stream\n");return false;}slot=(int)i;}
  if(((s2>>16)&3)==0)used[s2&127]=true;
 }
 if(slot<0)return false;
 int bank=-1;for(int i=(int)banks-2;i>=0;--i)if(!used[i] && (unsigned)i<sizeof(kCbSpi)/sizeof(kCbSpi[0]) && !(s[((pages+i)*4096)/4]&1u)){bank=i;break;}
 if(bank<0){IOLog("[FuxiWifi] no unused context bank\n");return false;}
 g.bank=(unsigned)bank;g.slot=(unsigned)slot;g.cb=(pages+g.bank)*4096;
 const unsigned bits[]={32,36,40,42,44,48};unsigned oas=(id2>>4)&15;if(oas>5)return false;
 g.config={32,bits[oas],g.tablePhys,gDmaBytes/4096};ArmSmmu4KRange range={kIova,g.phys,gDmaBytes,ArmSmmu4KRead|ArmSmmu4KWrite,1,2};
 int rc=gScattered?ArmSmmu4KBuild(&g.config,gRanges,gRangeCount,(uint64_t *)pool,kTablePages,&g.tables):ArmSmmu4KBuild(&g.config,&range,1,(uint64_t *)pool,kTablePages,&g.tables);
 if(rc){IOLog("[FuxiWifi] DMA table build error=%d ranges=%lu\n",rc,(unsigned long)(gScattered?gRangeCount:1));return false;}
 if(!gScattered){memset(g.va,0,gDmaBytes);clean(g.va,gDmaBytes);}   // the module window was zeroed and cleaned when it was reserved
 clean(pool,g.tables.table_pages*4096);
 g.context={&g.config,nullptr,&g.tables,nullptr,id0,id2,128+g.bank,0,0,1,0};
 g.io={(void *)s,0x100000,rd32,rd64,wr32,wr64,barrier};
 // The link is newly reset and endpoint bus mastering remains off. Only lease/fault WLAN SID 0x1401.
 uint32_t previous=s[(0xc00+4*g.slot)/4];wr32((void *)s,0xc00+4*g.slot,2u<<16);
 if(s[(0xc00+4*g.slot)/4]!=(2u<<16)){IOLog("[FuxiWifi] cannot fault owned stream (old=%x)\n",previous);return false;}
 ArmSmmu4KStream stream={g.slot,0x1401};ArmSmmu4KStreamResult sr={};
 rc=ArmSmmu4KCheckFaultedStreams(&g.io,&stream,1,g.bank,&sr);
 if(rc){IOLog("[FuxiWifi] SMMU preflight=%d scr0=%x fault=%x\n",rc,sr.scr0,sr.global_fault);return false;}
 if(!armcpu_spi_attach || armcpu_spi_attach(32+kCbSpi[g.bank],false,faultIrq,nullptr,nullptr,nullptr)){
  IOLog("[FuxiWifi] cannot attach owned context fault IRQ\n");return false;}
 ArmSmmu4KProgramResult pr={};rc=ArmSmmu4KProgramDisabledContext(&g.io,g.bank,&g.context,10000,&pr);
 if(!rc)rc=ArmSmmu4KAttachStreams(&g.io,&g.streams,&stream,1,g.bank,&g.context,&sr);
 IOLog("[FuxiWifi] DMA translated sid=1401 slot=%u bank=%u phys=%llx iova=%llx bytes=%u tables=%lu ranges=%lu wc=%u contiguous=%u result=%d\n",g.slot,g.bank,g.phys,kIova,gDmaBytes,(unsigned long)g.tables.table_pages,(unsigned long)(gScattered?gRangeCount:1),gScattered?1u:0u,gScattered&&gReservedContiguous?1u:0u,rc);
 return rc==0;
}
uint32_t barRead(void *p,uint32_t r){return ((volatile uint32_t *)p)[r/4];}
void barWrite(void *p,uint32_t r,uint32_t v){((volatile uint32_t *)p)[r/4]=v;__asm__ volatile("dsb sy":::"memory");}
void boot(volatile uint32_t *m,unsigned bytes){
 const uint8_t *fw=nullptr;size_t fwBytes=0;
 if(fuxi_wifi_firmware_get("amss.bin",&fw,&fwBytes) || fwBytes!=6090816 || !dmaPrepare())return;
 memcpy(g.va,fw,fwBytes);
 const unsigned tableOff=(fwBytes+4095)&~4095u,ctxOff=tableOff+4096,sinkOff=ctxOff+32768;
 uint64_t addresses[48];for(unsigned i=0;i<48;++i)addresses[i]=kIova+i*131072;
 unsigned entries=mhi_bhie_table((MhiBhieEntry *)(g.va+tableOff),256,addresses,(uint32_t)fwBytes,131072);
 if(!entries)return;
 Dma ctx={g.va+ctxOff,kIova+ctxOff,32768};
 uint32_t barLo=gCfg[4],barHi=gCfg[5];chipPowerUp(m);gCfg[4]=barLo;gCfg[5]=barHi;gCfg[1]=(gCfg[1]|2u)&~4u;endpointLinkSetup(kIova+sinkOff);
 MhiIo io={barRead,barWrite,(void *)m,bytes};MhiBoot state={};
 int opened=-1;unsigned layoutWait=0;
 for(;layoutWait<200;++layoutWait){opened=mhi_boot_open(&state,&io);if(!opened)break;IOSleep(10);}
 IOLog("[FuxiWifi] MHI layout bytes=%x reglen=%x ver=%x bhi=%x bhie=%x erdb=%x status=%x reset=%x/%x ee=%x waitMs=%u result=%d\n",
  bytes,m[0],m[2],m[0x28/4],m[0x2c/4],m[0x20/4],m[0x48/4],native_wifi_chip[8],native_wifi_chip[9],native_wifi_chip[10],layoutWait*10,opened);
 if(opened)return;
 if(!programContexts(m,ctx,bytes))return;
 clean(g.va,sinkOff+4096);
 // No SMMU bypass: firmware and contexts use a bounded translated 32-bit IOVA aperture.
 gDbi[1]=gDbi[1]|6u;gCfg[1]=gCfg[1]|6u;
 unsigned phase=mhi_boot_begin(&state,0,kIova,524288,kIova+tableOff,entries*sizeof(MhiBhieEntry),0x5eed),last=~0u,since=0;
 for(unsigned i=0;i<12000 && phase!=MhiDone && phase!=MhiFailed;++i){
  if(g.fault){IOLog("[FuxiWifi] DMA fault fsr=%x far=%llx; bus master stopped\n",g.fault,g.far);break;}
  if(phase!=last){last=phase;since=0;IOLog("[FuxiWifi] MHI phase=%u ee=%u state=%u error=%u\n",phase,state.ee,state.state,state.error);}
  if(phase==MhiNeedContexts){if(!programContexts(m,ctx,bytes)){gCfg[1]=gCfg[1]&~4u;break;}clean(ctx.va,ctx.bytes);phase=mhi_boot_contexts_done(&state);}
  else phase=mhi_boot_step(&state);
  clean(ctx.va,ctx.bytes);eventAck(m,bytes);
  if(++since>4000)phase=mhi_boot_timeout(&state);
  IOSleep(5);
 }
 IOLog("[FuxiWifi] MHI final phase=%u ee=%u state=%u error=%u failedPhase=%u; station/OS interface not started\n",state.phase,state.ee,state.state,state.error,state.failed_phase);
 if(state.phase!=MhiDone || g.fault)gCfg[1]=gCfg[1]&~4u;
 // Contexts, mappings, fault handler and parent PCIe windows must survive any uncertain in-flight DMA.
 for(;;)IOSleep(1000);
}
}
// fuxi-wifi=5: reserve the module's DMA window while memory is plentiful (fuxi_wifi_link_probe, at boot).
extern "C" void fuxi_wifi_dma_reserve(void){
 const bool ok=reserveModuleWindow(kModuleDmaBytes);
 IOLog("[FuxiWifi] module DMA window reserved=%u bytes=%u contiguous=%u ranges=%lu pa=%llx\n",ok?1u:0u,kModuleDmaBytes,gReservedContiguous?1u:0u,(unsigned long)gRangeCount,gReservedPa);
}
extern "C" void fuxi_wifi_mhi_start(volatile uint32_t *atu,volatile uint32_t *cfg,volatile uint32_t *dbi,volatile uint32_t *parf,bool coldReset){
 if(cfg[0]!=0x110717cb){IOLog("[FuxiWifi] refuse MHI for unexpected endpoint %x\n",cfg[0]);return;}
 gCfg=cfg;gDbi=dbi;cfg[1]=cfg[1]&~4u;
 constexpr uint64_t base=0x60300000,limit=0x64000000;
 volatile uint32_t *r=atu+0x200/4;r[2]=(uint32_t)base;r[3]=0;r[4]=(uint32_t)(limit-1);r[8]=0;r[5]=(uint32_t)base;r[6]=0;r[0]=0;r[1]=1u<<31;
 dbi[0x20/4]=(uint32_t)(((limit-1)&0xfff00000)|((base>>16)&0xfff0));
 uint32_t old=cfg[4];cfg[4]=~0u;uint32_t mask=cfg[4];cfg[4]=old;uint32_t size=~(mask&~15u)+1;
 if(!size||size>0x200000||!(mask&~15u)){IOLog("[FuxiWifi] BAR size refused mask=%x\n",mask);return;}
 uint32_t address=(uint32_t)((base+size-1)&~(uint64_t)(size-1));cfg[4]=address|(mask&15u);if((mask&6)==4)cfg[5]=0;cfg[1]=(cfg[1]|2u)&~4u;
 if(!mapWindow(g.bar,address,size)){IOLog("[FuxiWifi] BAR map failed\n");return;}
 // This backend cold-reset RC0 before PERST/link training. Keep its only
 // endpoint and root port non-mastering while initializing its private LUT.
 dbi[1]=dbi[1]&~4u;cfg[1]=cfg[1]&~4u;__asm__ volatile("dsb sy":::"memory");
 IOSleep(1);
 FuxiWifiSidIo sidIo={(void *)parf,
  [](void *p,unsigned o)->uint32_t{return ((volatile uint32_t *)p)[o/4];},
  [](void *p,unsigned o,uint32_t v){((volatile uint32_t *)p)[o/4]=v;__asm__ volatile("dsb sy":::"memory");}};
 unsigned oldNonzero=0;
 int sidRc=fuxi_wifi_sid_reset(&sidIo,coldReset,cfg[0],dbi[0x18/4],dbi[1],cfg[1],&oldNonzero);
 IOLog("[FuxiWifi] RC0 SID reset result=%d oldNonzero=%u hash=%u entry=%x cfg=%x rootCmd=%x epCmd=%x\n",
  sidRc,oldNonzero,fuxi_wifi_sid_hash(0x100),parf[(0x2000+4*fuxi_wifi_sid_hash(0x100))/4],parf[0x2c00/4],dbi[1],cfg[1]);
 if(sidRc)return;
 unsigned level=0;PE_parse_boot_argn("fuxi-wifi",&level,sizeof level);
 if(level==5){
  // The official path: Ath12kWifi does the chip power-up, MHI and firmware boot itself; the board gives it the BAR, config space and DMA window.
  IOLog("[FuxiWifi] stage3 BAR=%x bytes=%u sid=1401; Ath12kWifi module (DMA %u MiB uncached)\n",address,size,kModuleDmaBytes>>20);
  gDmaBytes=kModuleDmaBytes;gScattered=true;
  if(!dmaPrepare()){gCfg[1]=gCfg[1]&~4u;for(;;)IOSleep(1000);}
  // Memory decoding and bus mastering on both sides, as sheng's endpointProbe leaves them for wifiStack: the module's pci_enable_device and
  // pci_set_master are no-ops (Ath12kWifi src/nw_pci.c). Without the endpoint's own bus master its boot ROM never left the MHI reset of
  // SYS_ERR (193823: MHICTRL 2, MHISTATUS ff04 for the whole 90 s, power-up -110). The SMMU window and fault handler are in place by now.
  gDbi[1]=gDbi[1]|6u;gCfg[1]=(gCfg[1]&0xffffu)|6u;__asm__ volatile("dsb sy":::"memory");
  IOLog("[FuxiWifi] endpoint command %08x root command %08x\n",gCfg[1],gDbi[1]);
  fuxi_wifi_nw_run(g.bar.va,size,cfg,dbi,g.va,kIova,gDmaBytes,&g.fault,&g.far);
  gCfg[1]=gCfg[1]&~4u;for(;;)IOSleep(1000);
 }
 IOLog("[FuxiWifi] stage3 BAR=%x bytes=%u sid=1401 embedded c7-00108; starting bounded MHI boot\n",address,size);
 boot(g.bar.va,size);
 gCfg[1]=gCfg[1]&~4u;for(;;)IOSleep(1000); // retain DMA/fault-handler and parent windows after an early refusal
}
