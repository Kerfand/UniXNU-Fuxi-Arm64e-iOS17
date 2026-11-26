/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8550QupDma.h"
#include "ArmSmmu4K.h"
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif
static unsigned bits(unsigned code) {static const unsigned sizes[]={32,36,40,42,44,48};return code<6?sizes[code]:0;}
int sm8550_qup_dma_attach(const struct sm8550_qup_dma_io *io,struct sm8550_qup_dma *d,uint16_t sid,
    uint64_t pa,size_t bytes,uint64_t tpa,uint64_t *tables,size_t capacity) {
    if(!io || !io->read || !io->write || !io->delay_us || !io->publish || !d || !tables ||
        !bytes || bytes>0x200000 || (bytes&4095) || (pa&4095) || (tpa&4095) || sid>0x7fff || d->active || d->poisoned)return -EINVAL;
    uint32_t id0=io->read(io->context,0x20),id1=io->read(io->context,0x24),id2=io->read(io->context,0x28);
    d->id0=id0;d->id1=id1;d->id2=id2;
    d->free_slots=d->unused_banks=d->rejected_config=d->exhausted=0;
    d->rejected_cbar=d->rejected_cba2r=d->rejected_sctlr=d->rejected_fsr=0;
    unsigned slots=id0&255,banks=id1&255,pages=1u<<(((id1>>28)&7)+1);
    if(!(id0&(1u<<30)) || !(id0&(1u<<27)) || (id0&(1u<<8)) || !(id2&(1u<<12)) ||
        !slots || slots>128 || banks<2 || banks>128 || (id1>>31) || (pages+banks)*4096>0xd0000)return -ENOTSUP;
    uint32_t scr0=io->read(io->context,0);
    if((scr0&1) || !(scr0&(1u<<10)))return -ENOTSUP;
    bool used[128]={false};unsigned slot=slots,bank=banks,asid=0;
    for(unsigned i=0;i<slots;i++) {
        uint32_t smr=io->read(io->context,0x800+i*4),s2cr=io->read(io->context,0xc00+i*4);
        if(!(smr&(1u<<31))) {d->free_slots++;if(slot==slots)slot=i;continue;}
        if(((s2cr>>16)&3)==0 && (s2cr&255)<banks)used[s2cr&255]=true;
        if(!((smr^sid)&0x7fff&~((smr>>16)&0x7fff)))return -EBUSY;
    }
    if(slot==slots){d->exhausted=1;return -ENOSPC;}
    for(unsigned i=1;i<banks;i++) {
        unsigned cb=(pages+i)*4096;
        if(used[i])continue;
        d->unused_banks++;
        uint32_t cbar=io->read(io->context,0x1000+i*4);
        if(cbar){d->rejected_config++;d->rejected_cbar=cbar;continue;}
        uint32_t cba2r=io->read(io->context,0x1800+i*4);
        if(cba2r){d->rejected_config++;d->rejected_cba2r=cba2r;continue;}
        d->rejected_sctlr=io->read(io->context,cb);
        if(d->rejected_sctlr&1u)continue;
        d->rejected_fsr=io->read(io->context,cb+0x58);
        if(!(d->rejected_fsr&0xc00001feu)){bank=i;break;}
    }
    if(bank==banks){d->exhausted=2;return -ENOSPC;}
    for(unsigned candidate=1;candidate<256;candidate++) {
        bool taken=false;
        for(unsigned i=0;i<banks;i++)if(used[i] || (io->read(io->context,(pages+i)*4096)&1)) {
            if((io->read(io->context,(pages+i)*4096+0x24)>>16)==candidate ||
                (io->read(io->context,(pages+i)*4096+0x2c)>>16)==candidate){taken=true;break;}
        }
        if(!taken){asid=candidate;break;}
    }
    if(!asid){d->exhausted=3;return -ENOSPC;}
    unsigned output=bits((id2>>4)&15);if(!output)return -ENOTSUP;
    ArmSmmu4KConfig cfg={32,output,tpa,bytes/4096};
    ArmSmmu4KRange range={SM8550_QUP_DMA_IOVA,pa,bytes,ArmSmmu4KRead|ArmSmmu4KWrite,1,3};
    ArmSmmu4KResult result;
    if(ArmSmmu4KBuild(&cfg,&range,1,tables,capacity,&result)!=ArmSmmu4KSuccess)return -EINVAL;
    if(result.start_level!=1)return -EINVAL;
    io->publish(io->context,tables,result.table_pages*4096);
    unsigned cb=(pages+bank)*4096;
    d->bank=bank;d->slot=slot;d->asid=asid;d->pages=pages;d->poisoned=true;
    io->write(io->context,0x1800+bank*4,1);
    io->write(io->context,0x1000+bank*4,0x1f300);
    io->write(io->context,cb+0x10,((id2>>4)&15)|(7u<<15));
    io->write(io->context,cb+0x30,32u|(3u<<12)|(1u<<23));
    io->write(io->context,cb+0x20,(uint32_t)tpa);
    io->write(io->context,cb+0x24,(uint32_t)(tpa>>32)|(asid<<16));
    io->write(io->context,cb+0x28,0);io->write(io->context,cb+0x2c,0);
    io->write(io->context,cb+0x38,(uint32_t)result.mair);io->write(io->context,cb+0x3c,(uint32_t)(result.mair>>32));
    io->write(io->context,cb+0x618,0);io->write(io->context,cb+0x7f0,0);
    unsigned poll=0;
    for(;poll<1000 && (io->read(io->context,cb+0x7f4)&1);poll++)io->delay_us(io->context,1);
    if(poll==1000)return -ETIMEDOUT;
    io->write(io->context,cb,0x00c01027);
    if(io->read(io->context,cb)!=0x00c01027 || io->read(io->context,cb+0x20)!=(uint32_t)tpa ||
        io->read(io->context,cb+0x24)!=((uint32_t)(tpa>>32)|(asid<<16)))return -EIO;
    io->write(io->context,0xc00+slot*4,bank);
    io->write(io->context,0x800+slot*4,0x80000000u|sid);
    if(io->read(io->context,0xc00+slot*4)!=bank || io->read(io->context,0x800+slot*4)!=(0x80000000u|sid)) {
        io->write(io->context,0xc00+slot*4,2u<<16);return -EIO;
    }
    d->active=true;d->poisoned=false;return 0;
}
