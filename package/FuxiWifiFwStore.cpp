// SPDX-License-Identifier: BSD-2-Clause-Patent
// The embedded WCN7850 firmware (FuxiWifiFirmware.S) is stored as zlib streams; each file is inflated once, on its first request, into memory
// kept for the boot. The stream's adler32 and the exact length (wifi-firmware/manifest.json) are checked. Shared by both Ath12kWifi board
// files (FuxiWifiNw.cpp for the mainline runtime, FuxiWifiNw21.cpp for the 21H461 one) and the MhiBoot path (FuxiWifiMhi.cpp, fuxi-wifi=3).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <string.h>
#include <libkern/zlib.h>
extern "C" const unsigned char fuxi_wifi_amss_z_start[],fuxi_wifi_amss_z_end[],fuxi_wifi_m3_z_start[],fuxi_wifi_m3_z_end[];
extern "C" const unsigned char fuxi_wifi_board2_z_start[],fuxi_wifi_board2_z_end[];

namespace {
struct EmbeddedFw {const char *name;const unsigned char *z,*zEnd;size_t bytes;uint8_t *data;};
EmbeddedFw gEmbedded[]={{"amss.bin",fuxi_wifi_amss_z_start,fuxi_wifi_amss_z_end,6090816,nullptr},
                        {"m3.bin",fuxi_wifi_m3_z_start,fuxi_wifi_m3_z_end,299660,nullptr},
                        {"board-2.bin",fuxi_wifi_board2_z_start,fuxi_wifi_board2_z_end,2431968,nullptr}};
voidpf zAlloc(voidpf,uInt items,uInt size){
 const size_t n=(size_t)items*size+16;uint8_t *p=static_cast<uint8_t *>(IOMallocData(n));
 if(!p)return nullptr;*reinterpret_cast<size_t *>(p)=n;return p+16;
}
void zFree(voidpf,voidpf q){if(!q)return;uint8_t *p=static_cast<uint8_t *>(q)-16;IOFreeData(p,*reinterpret_cast<size_t *>(p));}
}
// Returns 0, -2 for a file not embedded, other negative values for a failed inflate.
extern "C" int fuxi_wifi_firmware_get(const char *name,const uint8_t **data,size_t *size){
 static IOLock *lock;
 if(!__atomic_load_n(&lock,__ATOMIC_ACQUIRE)){
  IOLock *created=IOLockAlloc(),*expected=nullptr;if(!created)return -12;
  if(!__atomic_compare_exchange_n(&lock,&expected,created,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))IOLockFree(created);
 }
 EmbeddedFw *f=nullptr;
 for(EmbeddedFw &e:gEmbedded)if(!strcmp(e.name,name))f=&e;
 if(!f)return -2;
 IOLockLock(lock);
 int rc=0;
 if(!f->data){
  uint8_t *out=static_cast<uint8_t *>(IOMallocData(f->bytes));
  if(!out)rc=-12;
  else{
   z_stream s;memset(&s,0,sizeof s);s.zalloc=zAlloc;s.zfree=zFree;
   s.next_in=const_cast<Bytef *>(f->z);s.avail_in=(uInt)(f->zEnd-f->z);s.next_out=out;s.avail_out=(uInt)f->bytes;
   rc=inflateInit(&s);
   if(rc==Z_OK){rc=inflate(&s,Z_FINISH);rc=rc==Z_STREAM_END&&s.total_out==f->bytes&&s.avail_in==0?0:(rc==Z_STREAM_END?-3:rc<0?rc:-4);inflateEnd(&s);}
   else rc=rc<0?rc:-5;
   if(rc)IOFreeData(out,f->bytes);else f->data=out;
  }
 }
 IOLockUnlock(lock);
 if(rc)return rc;
 *data=f->data;*size=f->bytes;return 0;
}
