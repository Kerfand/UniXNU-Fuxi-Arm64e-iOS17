/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SM8550_QUP_DMA_H
#define SM8550_QUP_DMA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SM8550_QUP_DMA_IOVA 0x40000000ULL
struct sm8550_qup_dma_io {
    void *context;
    uint32_t (*read)(void *,uint32_t);
    void (*write)(void *,uint32_t,uint32_t);
    void (*delay_us)(void *,unsigned);
    void (*publish)(void *,void *,size_t);
};
struct sm8550_qup_dma {
    unsigned bank,slot,asid,pages;
    /* Selection evidence from existing reads, with no diagnostic MMIO scan.
     * exhausted: 1 stream slots, 2 context banks, 3 ASIDs. */
    uint32_t id0,id1,id2,rejected_cbar,rejected_cba2r,rejected_sctlr,rejected_fsr;
    unsigned free_slots,unused_banks,rejected_config,exhausted;
    bool active,poisoned;
};
/* Master is idle and caller holds the apps-SMMU configuration lock.
 * Buffers and page tables stay allocated for this domain's entire lifetime. */
int sm8550_qup_dma_attach(const struct sm8550_qup_dma_io *,struct sm8550_qup_dma *,uint16_t sid,
    uint64_t buffer_pa,size_t bytes,uint64_t tables_pa,uint64_t *tables,size_t table_pages);
bool sm8550_apps_smmu_lock(void);
void sm8550_apps_smmu_unlock(void);
#ifdef __cplusplus
}
#endif
#endif
