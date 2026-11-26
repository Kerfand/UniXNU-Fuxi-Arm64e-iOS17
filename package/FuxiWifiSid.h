// SPDX-License-Identifier: BSD-2-Clause-Patent
// RC0-local reset table, not the shared Apps SMMU stream table.
#pragma once
#include <stdint.h>
struct FuxiWifiSidIo {void *cookie;uint32_t (*read)(void *,unsigned);void (*write)(void *,unsigned,uint32_t);};
static inline unsigned fuxi_wifi_sid_hash(uint16_t bdf) {
 unsigned crc=0;for(int shift=8;shift>=0;shift-=8){crc^=(bdf>>shift)&255;for(unsigned bit=0;bit<8;++bit)crc=(crc&128)?((crc<<1)^7)&255:(crc<<1)&255;}return crc;
}
// Destructive reset initialization is permitted only for the RC this backend
// cold-reset itself, the sole known endpoint, and with both bus masters off.
static inline int fuxi_wifi_sid_reset(const FuxiWifiSidIo *io,bool coldReset,
 uint32_t endpoint,uint32_t buses,uint32_t rootCmd,uint32_t epCmd,unsigned *oldNonzero) {
 if(!io||!io->read||!io->write||!coldReset||endpoint!=0x110717cb||
    (buses&0xffffff)!=0x010100||(rootCmd&4)||(epCmd&4))return -1;
 unsigned occupied=0;
 for(unsigned i=0;i<256;++i)occupied+=io->read(io->cookie,0x2000+4*i)!=0;
 if(oldNonzero)*oldNonzero=occupied;
 // Matches qcom_pcie_config_sid_1_9_0: reset contents are not valid leases.
 for(unsigned i=0;i<256;++i)io->write(io->cookie,0x2000+4*i,0);
 for(unsigned i=0;i<256;++i)if(io->read(io->cookie,0x2000+4*i))return -2;
 const unsigned hash=fuxi_wifi_sid_hash(0x100),entry=(0x100u<<16)|(1u<<8);
 io->write(io->cookie,0x2000+4*hash,entry);
 if(io->read(io->cookie,0x2000+4*hash)!=entry)return -3;
 uint32_t cfg=io->read(io->cookie,0x2c00);io->write(io->cookie,0x2c00,cfg&~1u);
 if(io->read(io->cookie,0x2c00)!=(cfg&~1u))return -4;
 return 0;
}
