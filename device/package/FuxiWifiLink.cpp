// SPDX-License-Identifier: BSD-2-Clause-Patent
// PCIe0 link bring-up adapted from UniXNU-sheng 516a780473a2ac59c8b438e132bb60204dda9e22.
// Fuxi-specific rails; bounded RPMh completion; stop after endpoint identification.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "WifiPcieTables.h"
#include "QcomRpmh.h"
#include "BootResetCore.h"
#include "FuxiWifiClock.h"
extern "C" void fuxi_wifi_mhi_start(volatile uint32_t *,volatile uint32_t *,volatile uint32_t *,volatile uint32_t *,bool);
extern "C" void fuxi_spmi_lock(void);extern "C" void fuxi_spmi_unlock(void);
extern "C" void fuxi_wifi_dma_reserve(void);   // FuxiWifiMhi.cpp
namespace {
volatile uint32_t native_wifi_stats[64],native_wifi_pmic[512];
constexpr uint64_t kGcc = 0x100000, kTlmm = 0xf100000, kParf = 0x1c00000, kDbi = 0x60000000, kPhy = 0x1c06000, kTcsrClkref = 0x1fd5000;
constexpr uint64_t kAtu = 0x60001000, kConfig = 0x60100000;
constexpr uint32_t kVote8 = 0x52008, kVote0 = 0x52000, kGdsc = 0x6b004, kPhyGdsc = 0x6c000, kCollapse = 0x52020;
// slv_axi, mstr_axi, cfg_ahb, aux, pipe, slv_q2a, cnoc_sf, aggre_noc, ddrss_sf, phy_rchng
constexpr uint32_t kBranches[] = {0x6b020, 0x6b02c, 0x6b038, 0x6b03c, 0x6b048, 0x6b01c, 0x10030, 0x1003c, 0x1004c, 0x6b054};
// constexpr uint32_t kNeeded = 0xf;   // slv_axi, mstr_axi, cfg_ahb, aux
constexpr unsigned kGpios[] = {80, 82, 94};
constexpr uint32_t kParfLtssm = 0x1b0, kParfPhyCtrl = 0x40, kLinkControl = 0x80;
constexpr unsigned kWlanEn = 80, kPerst = 94, kClkReq = 95;
// gcc-sm8550.c: block resets, the pipe source mux (0 PHY, 2 TCXO) and the aux (TCXO) and rchng (GPLL0 even / 3) sources
constexpr uint32_t kPcieBcr = 0x6b000, kPhyBcr = 0x6c01c, kPipeMux = 0x6b070, kAuxRcg = 0x6b074, kRchngRcg = 0x6b058;
constexpr uint32_t kVote8Bits = 0x7f, kVote0Bits = 1u << 12 | 1u << 19 | 1u << 22;
// pcie-qcom.c
constexpr uint32_t kParfSysCtrl = 0x0, kParfPmCtrl = 0x20, kParfDbiBase = 0x350, kParfDbiBaseHi = 0x354, kParfMhiClockReset = 0x174;
constexpr uint32_t kParfAxiHalt = 0x1a8, kParfAtuBase = 0x634, kParfAtuBaseHi = 0x638, kParfDeviceType = 0x1000;
constexpr uint32_t kParfSlvSpace = 0x358, kParfSlvSpaceHi = 0x35c, kSlvSpaceBytes = 0x80000000;
// pcie-designware.h
constexpr uint32_t kPortDebug0 = 0x728, kPortDebug1 = 0x72c, kBusNumbers = 0x18, kCommand = 0x4;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void release(Window &w) {if (w.map) w.map->release(); w = {};}

// TLMM pin config: pull in bits 0-1 (1 down, 3 up), function in 2-5, drive (mA / 2 - 1) in 6-8, output enable bit 9; io bit 1 drives
void setPin(volatile uint32_t *tlmm, unsigned pin, uint32_t config, int level) {
    if (level >= 0) tlmm[(pin * 0x1000 + 4) / 4] = level ? 2u : 0u;
    tlmm[pin * 0x1000 / 4] = config;
    if (level >= 0) tlmm[(pin * 0x1000 + 4) / 4] = level ? 2u : 0u;
}

uint32_t pinWord(volatile uint32_t *tlmm) {
    uint32_t gpios = 0;
    for (unsigned i = 0; i < 3; ++i) gpios |= (tlmm[(kGpios[i] * 0x1000 + 4) / 4] & 3u) << (8 * i);
    return gpios;
}

bool waitClear(volatile uint32_t *reg, uint32_t mask, unsigned tries, unsigned *used = nullptr) {
    unsigned i = 0;
    for (; i < tries && (*reg & mask); ++i) IODelay(100);
    if (used) *used = i;
    return !(*reg & mask);
}

void pulseReset(volatile uint32_t *gcc, uint32_t bcr) {
    gcc[bcr / 4] |= 1u;
    IODelay(1000);
    gcc[bcr / 4] &= ~1u;
    IODelay(1000);
}


bool prepareSleepClock(){
 Window core={},cnfg={},obs={},chn={};bool ok=false;
 if(!mapWindow(core,0xc400000,0x3000)||!mapWindow(cnfg,0xc42d000,0x4000))goto done;
 {
  br_arbiter arb={core.va,cnfg.va,0};uint32_t count=core.va[BR_FEATURES/4]&BR_FEATURES_APIDS_MASK;
  if(core.va[0]<BR_VERSION_V7_MIN||core.va[0]>=BR_VERSION_V8_MIN||!count||count>1024)goto done;
  if(!mapWindow(obs,0xc440000,((uint64_t)count*BR_OBS_APID_STRIDE+4095)&~4095ull)||!mapWindow(chn,0xc500000,(uint64_t)count*BR_CHNL_APID_STRIDE))goto done;
  int apid=-1;
  for(uint32_t i=0;i<count;++i){uint32_t m=core.va[(BR_APID_MAP+4*i)/4];
   if(m && ((m>>8)&0xfff)==0xba && (cnfg.va[i]&7)==0)apid=(int)i;}
  if(apid<0){IOLog("[FuxiWifi] sleep-clock GPIO3 not owned by EE0; refusing write\n");goto done;}
  uint8_t sub=0;fuxi_spmi_lock();int rc=br_read8(obs.va,&arb,apid,5,&sub);fuxi_spmi_unlock();
  if(rc||sub<0x10||sub>0x13){IOLog("[FuxiWifi] sleep-clock subtype=%x rc=%d unsupported\n",(unsigned)sub,rc);goto done;}
  struct ClockSession {volatile uint32_t *obs,*chn;int apid;};ClockSession session={obs.va,chn.va,apid};
  FuxiWifiClockIo io={&session,
   [](void *p,uint8_t reg,uint8_t *value,uint32_t *status)->int{
    auto *s=(ClockSession *)p;volatile uint32_t *ch=s->obs+(BR_OBS_APID_STRIDE*(uint32_t)s->apid)/4;
    ch[BR_CMD/4]=br_cmd(BR_OP_EXT_READL,reg,0);__asm__ volatile("dsb sy":::"memory");
    int rc=br_wait(ch);*status=ch[BR_STATUS/4];__asm__ volatile("dsb sy":::"memory");
    if(!rc)*value=(uint8_t)ch[BR_RDATA0/4];return rc;},
   [](void *p,uint8_t reg,uint8_t value,uint32_t *status)->int{
    auto *s=(ClockSession *)p;volatile uint32_t *ch=s->chn+(BR_CHNL_APID_STRIDE*(uint32_t)s->apid)/4;
    ch[BR_WDATA0/4]=value;__asm__ volatile("dsb sy":::"memory");
    ch[BR_CMD/4]=br_cmd(BR_OP_EXT_WRITEL,reg,0);__asm__ volatile("dsb sy":::"memory");
    int rc=br_wait(ch);*status=ch[BR_STATUS/4];return rc;},
   [](void *,uint8_t reg,uint8_t before,uint8_t wanted,uint8_t after,unsigned attempt,int wr,int rr,uint32_t ws,uint32_t rs){
    IOLog("[FuxiWifi] sleep-clock reg=%x before=%x want=%x after=%x attempt=%u writeRc=%d readRc=%d writeStatus=%x readStatus=%x\n",
      reg,before,wanted,after,attempt,wr,rr,ws,rs);},
   [](void *){IODelay(20);}};
  // The button poller shares the arbiter (225921: its read failed in the same window as this write); hold the SPMI lock for the session.
  fuxi_spmi_lock();rc=fuxi_wifi_sleep_clock(&io);fuxi_spmi_unlock();
  ok=rc>=0;IOLog("[FuxiWifi] sleep-clock apid=%d subtype=%x verified=%u rc=%d\n",apid,(unsigned)sub,ok?1u:0u,rc);

 }
 done:release(chn);release(obs);release(cnfg);release(core);return ok;
}
// Rail names and voltages come from fuxi.dtb chip_cfg@1/1107 (not sheng's voltage table).
struct Rail{const char *name;uint32_t mv;};
constexpr Rail kRails[]={{"smpg5",1000},{"ldob15",1800},{"smpg2",980},{"smpe4",950},{"smpg4",1350},{"smpg6",1900}};
int powerRails(){
 static uint8_t db[0x20000];Window dw={},rw={};
 if(!mapWindow(dw,0x81c60000,sizeof db))return -1;
 for(unsigned i=0;i<sizeof db;i+=4){uint32_t x=dw.va[i/4];memcpy(db+i,&x,4);}release(dw);
 QcomRpmhResource rails[6];
 for(unsigned i=0;i<6;++i)if(qcom_cmd_db_find(db,sizeof db,kRails[i].name,&rails[i])){IOLog("[FuxiWifi] missing cmd-db rail %s\n",kRails[i].name);return -2;}
 if(!mapWindow(rw,0x17a20000,0x10000))return -3;
 volatile uint32_t *r=rw.va;const QcomRscLayout l=qcom_rsc_layout(r[0]);const unsigned tcs=0;const unsigned base=0xd00;
 // TCS2 is used by touch. Refuse an already triggered/enabled TCS0 rather than overwrite it.
 if((r[(base+l.control)/4]&(1u<<24))||r[(base+l.cmd_enable)/4]){IOLog("[FuxiWifi] RPMh TCS0 busy; abort\n");release(rw);return -4;}
 const uint32_t irqEnable=r[base/4];r[base/4]=irqEnable|1u;
 auto send=[&](uint32_t addr,uint32_t value){
  QcomRpmhWrite w[16];unsigned n=qcom_rpmh_active_request(&l,tcs,&addr,&value,1,w,16);
  if(!n)return false;
  for(unsigned i=0;i<n;++i)r[(base+w[i].offset)/4]=w[i].value;
  __asm__ volatile("dsb sy":::"memory");
  unsigned tries=0;for(;tries<1000;++tries){if(r[(base+l.irq_status)/4]&1u)break;IODelay(10);}
  if(tries==1000){IOLog("[FuxiWifi] RPMh timeout addr=%x value=%u status=%x\n",addr,value,r[(base+l.status)/4]);return false;}
  n=qcom_rpmh_finish(&l,tcs,w,16);for(unsigned i=0;i<n;++i)r[(base+w[i].offset)/4]=w[i].value;
  return true;
 };
 int rc=0;
 for(unsigned i=0;i<6 && !rc;++i){
  bool ok=send(rails[i].address+8,7)&&send(rails[i].address,kRails[i].mv)&&send(rails[i].address+4,1);
  IOLog("[FuxiWifi] rail %s address=%x mv=%u completed=%u\n",kRails[i].name,rails[i].address,kRails[i].mv,ok?1u:0u);
  if(!ok)rc=-5;
 }
 // Keep the IRQ enabled if a timed-out TCS is still in flight; never clear another owner's completion.
 if(!rc){r[base/4]=irqEnable;native_wifi_stats[20]=0x3f;}
 release(rw);return rc;
}
void bringUp(Window &gcc, Window &tlmm) {
    volatile uint32_t *g = gcc.va, *t = tlmm.va;
    // As sheng's pmicPrepare: the result is recorded, not a gate (221653/224516 trained the link after the same pin setup).
    if(!prepareSleepClock())IOLog("[FuxiWifi] sleep-clock not verified; continuing with the rails\n");
    const int rails = powerRails();
    IOSleep(20);

    if (rails) {native_wifi_stats[0] = -9; return;}
    IOSleep(5);
    // PERST# held low, CLKREQ# to its PCIe function, then WLAN_EN high and the WCN7850 power-up delay (pwrseq-qcom-wcn.c: 50 ms)
    setPin(t, kPerst, 1u | 1u << 9, 0);
    setPin(t, kClkReq, 3u | 1u << 2, -1);
    // the chip may still run what an earlier start left: WLAN_EN goes low first so that it starts from nothing
    setPin(t, kWlanEn, 1u | 3u << 6 | 1u << 9, 0);
    IOSleep(150);
    setPin(t, kWlanEn, 1u | 3u << 6 | 1u << 9, 1);
    IOSleep(60);
    native_wifi_stats[10] = pinWord(t);
    // what the pin registers hold now (a write TZ or the hypervisor drops reads back as written-nothing)
    native_wifi_stats[17] = (t[kWlanEn * 0x1000 / 4] & 0xffff) | t[(kWlanEn * 0x1000 + 4) / 4] << 16;
    native_wifi_stats[18] = (t[kPerst * 0x1000 / 4] & 0xffff) | t[(kPerst * 0x1000 + 4) / 4] << 16;
    native_wifi_stats[19] = (t[kClkReq * 0x1000 / 4] & 0xffff) | t[(kClkReq * 0x1000 + 4) / 4] << 16;
    native_wifi_stats[0] = 10;

    // both GDSCs are votable: a set bit in the collapse register (PCIe0 bit 0, PHY bit 3) keeps them collapsed
    g[kCollapse / 4] &= ~(1u << 0 | 1u << 3);
    unsigned tries = 0;
    while (tries++ < 100 && (!(g[kGdsc / 4] & (1u << 31)) || !(g[kPhyGdsc / 4] & (1u << 31)))) IODelay(10);
    uint32_t power = (g[kGdsc / 4] >> 31) | (g[kPhyGdsc / 4] >> 31) << 1;
    native_wifi_stats[11] = power;
    if (power != 3) {native_wifi_stats[0] = -11; return;}
    native_wifi_stats[0] = 11;

    // sources first (an RCG takes a new CFG on CMD bit 0), the pipe mux parked on TCXO until the PHY runs, then the branch votes
    g[(kAuxRcg + 4) / 4] = 0;
    g[kAuxRcg / 4] |= 1u;
    g[(kRchngRcg + 4) / 4] = 6u << 8 | 5u;
    g[kRchngRcg / 4] |= 1u;
    waitClear(&g[kAuxRcg / 4], 1u, 100);
    waitClear(&g[kRchngRcg / 4], 1u, 100);
    g[kPipeMux / 4] = (g[kPipeMux / 4] & ~3u) | 2u;
    g[kVote8 / 4] |= kVote8Bits;
    g[kVote0 / 4] |= kVote0Bits;
    Window clkref = {};
    if (!mapWindow(clkref, kTcsrClkref, 0x1000)) {native_wifi_stats[0] = -12; return;}
    clkref.va[0x100 / 4] |= 1u;
    release(clkref);
    IODelay(200);
    uint32_t off = 0;
    for (unsigned i = 0; i < sizeof kBranches / sizeof kBranches[0]; ++i)
        if (g[kBranches[i] / 4] & (1u << 31)) off |= 1u << i;
    native_wifi_stats[11] = power | off << 8;
    if (off & ~(1u << 4)) {native_wifi_stats[0] = -12; return;}   // the pipe branch runs only once the PHY drives it
    pulseReset(g, kPcieBcr);
    pulseReset(g, kPhyBcr);
    native_wifi_stats[0] = 12;

    // qcom_pcie_init_2_7_0 after its resets, before the PHY powers on (qcom_pcie_host_init): PHY_CTRL's test power-down bit holds the PHY
    Window parf = {}, dbi = {};
    if (!mapWindow(parf, kParf, 0x3000) || !mapWindow(dbi, kDbi, 0x1000)) {release(parf); native_wifi_stats[0] = -13; return;}
    volatile uint32_t *f = parf.va, *d = dbi.va;
    f[kParfDeviceType / 4] = 4u;
    f[kParfPhyCtrl / 4] &= ~1u;
    f[kParfDbiBase / 4] = uint32_t(kDbi);
    f[kParfDbiBaseHi / 4] = uint32_t(kDbi >> 32);
    f[kParfAtuBase / 4] = uint32_t(kAtu);
    f[kParfAtuBaseHi / 4] = uint32_t(kAtu >> 32);
    f[kParfSlvSpace / 4] = 0;                 // qcom_pcie_configure_dbi_atu_base: the slave window spans 2 GiB << 32
    f[kParfSlvSpaceHi / 4] = kSlvSpaceBytes;
    f[kParfSysCtrl / 4] &= ~(1u << 29);
    f[kParfMhiClockReset / 4] |= 1u << 4;
    f[kParfPmCtrl / 4] &= ~(1u << 5);
    f[kParfAxiHalt / 4] |= 1u << 31;
    native_wifi_stats[0] = 13;

    // qmp_pcie_power_on: power-down control, the tables (both lanes), pipe clock from the PHY, out of reset, start, PHYSTATUS low
    Window phy = {};
    if (!mapWindow(phy, kPhy, 0x2000)) {release(dbi); release(parf); native_wifi_stats[0] = -14; return;}
    volatile uint32_t *p = phy.va;
    using namespace wifi_phy;
    p[kPowerDownControl / 4] |= 3u;
    for (const Init &e : kSerdes) p[e.offset / 4] = e.value;
    for (const Init &e : kTx) {p[e.offset / 4] = e.value; p[(e.offset + kTx2) / 4] = e.value;}
    for (const Init &e : kRx) {p[e.offset / 4] = e.value; p[(e.offset + kRx2) / 4] = e.value;}
    for (const Init &e : kPcs) p[e.offset / 4] = e.value;
    for (const Init &e : kPcsMisc) p[e.offset / 4] = e.value;
    g[kPipeMux / 4] &= ~3u;
    p[kSwReset / 4] &= ~1u;
    p[kStartControl / 4] |= 3u;
    IODelay(1000);
    unsigned polls = 0;
    const bool ready = waitClear(&p[kPcsStatus1 / 4], 1u << 6, 100, &polls);
    native_wifi_stats[12] = (p[kPcsStatus1 / 4] & 0xffffu) | polls << 16;
    native_wifi_pmic[17] = g[kBranches[4] / 4];
    release(phy);
    if (!ready) {release(dbi); release(parf); native_wifi_stats[0] = -14; return;}

    // No endpoint DMA before chip identification; keep SMMU stream tables untouched.
    // qcom_pcie_post_init_2_7_0 and the root port's bus numbers and command (dw_pcie_setup_rc)
    d[kBusNumbers / 4] = 0x00010100;   // primary 0, secondary 1, subordinate 1
    d[kCommand / 4] = (d[kCommand / 4] | 0x2u) & ~0x4u;           // memory decoding only; bus master remains off
    native_wifi_stats[7] = d[0];
    native_wifi_stats[0] = 14;

    // qcom_ep_reset_deassert: PERST# low for at least 100 ms, then high, then the LTSSM (qcom_pcie_2_3_2_ltssm_enable)
    IOSleep(100);
    setPin(t, kPerst, 1u | 1u << 9, 1);
    IODelay(1000);
    native_wifi_stats[0] = 15;
    f[kParfLtssm / 4] |= 1u << 8;
    const uint64_t start = mach_absolute_time();
    unsigned ms = 0;
    for (; ms < 1000; ++ms) {
        if ((d[kPortDebug1 / 4] & (1u << 4)) && (d[kPortDebug0 / 4] & 0x3f) == 0x11) break;
        if (ms == 20) {native_wifi_pmic[19] = d[kPortDebug0 / 4]; native_wifi_pmic[21] = d[kPortDebug1 / 4];}
        IOSleep(1);
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    native_wifi_stats[16] = uint32_t(ns / 1000000);
    native_wifi_pmic[18] = (t[kPerst * 0x1000 / 4] & 0xffff) | t[(kPerst * 0x1000 + 4) / 4] << 16;
    native_wifi_pmic[20] = g[kPipeMux / 4];
    native_wifi_stats[13] = d[kPortDebug0 / 4];
    native_wifi_stats[14] = d[kPortDebug1 / 4];
    native_wifi_stats[6] = f[kParfLtssm / 4];
    native_wifi_stats[8] = d[kLinkControl / 4];
    native_wifi_stats[9] = f[kParfPhyCtrl / 4];
    if (ms == 1000) {release(dbi); release(parf); native_wifi_stats[0] = -16; return;}
    native_wifi_stats[0] = 16;

    // outbound iATU region 0 (unrolled at the ATU base) turns the 1 MiB config window into type 0 accesses to bus 1 device 0
    Window atu = {}, config = {};
    if (mapWindow(atu, kAtu, 0x1000) && mapWindow(config, kConfig, 0x1000)) {
        volatile uint32_t *a = atu.va;
        a[0x08 / 4] = uint32_t(kConfig);
        a[0x0c / 4] = uint32_t(kConfig >> 32);
        a[0x10 / 4] = uint32_t(kConfig + 0xfffff);
        a[0x20 / 4] = uint32_t((kConfig + 0xfffff) >> 32);
        a[0x14 / 4] = 1u << 24;
        a[0x18 / 4] = 0;
        a[0x00 / 4] = 0x4;
        a[0x04 / 4] = 1u << 31;
        for (unsigned i = 0; i < 5 && !(a[0x04 / 4] & (1u << 31)); ++i) IODelay(10);
        native_wifi_stats[15] = config.va[0];
        native_wifi_stats[0] = 17;
        IOLog("[FuxiWifi] endpoint vendor/device=%08x\n",native_wifi_stats[15]);
        unsigned level=0;PE_parse_boot_argn("fuxi-wifi",&level,sizeof level);
        if(level==3||level==5)fuxi_wifi_mhi_start(a,config.va,d,parf.va,true);   // 5: Ath12kWifi module (FuxiWifiNw.cpp)
    } else native_wifi_stats[0] = -17;
    release(config); release(atu); release(dbi); release(parf);
}


void wifiLinkThread(void *,wait_result_t){
 thread_set_thread_name(current_thread(),"fuxi-wifi-link");IOSleep(65000);
 IOLog("[FuxiWifi] stage2 begin RC0 power/clock/PHY/link; chip ID will be verified before firmware/DMA\n");
 Window gcc={},tlmm={};
 if(!mapWindow(gcc,kGcc,0x80000)||!mapWindow(tlmm,kTlmm,0x60000)){release(gcc);release(tlmm);IOLog("[FuxiWifi] stage2 map failed\n");return;}
 bringUp(gcc,tlmm);
 IOLog("[FuxiWifi] stage2 result=%d rails=%x powerClocks=%x phy=%x ltssm=%x/%x endpoint=%x linkMs=%u\n",(int)native_wifi_stats[0],native_wifi_stats[20],native_wifi_stats[11],native_wifi_stats[12],native_wifi_stats[13],native_wifi_stats[14],native_wifi_stats[15],native_wifi_stats[16]);
 release(gcc);release(tlmm);
}
}
extern "C" void fuxi_wifi_link_probe(void){
 static bool started;unsigned stage=0;
 if(!PE_parse_boot_argn("fuxi-wifi",&stage,sizeof stage)||(stage<2||stage>5||stage==4)||started)return;started=true;
 if(stage==5)fuxi_wifi_dma_reserve();
 thread_t t=nullptr;if(kernel_thread_start(wifiLinkThread,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);
 else IOLog("[FuxiWifi] stage2 worker creation failed\n");
}
