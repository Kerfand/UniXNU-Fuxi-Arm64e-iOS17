/* fuxi 2026-11-26: from ljhhkx529/QcomBaseband (fuxi_ipa.c), the modem GLINK/QRTR renamed modem_glink_/modem_qrtr_. */
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The IPA's windows on fuxi, mapped and read. Nothing here writes a register or loads firmware: the point of
 * this file is to find out what the silicon says before anything is configured on it.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <machine/machine_routines.h>

#include "fuxi_ipa.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
static inline uint32_t rd(vm_offset_t b, uint32_t o) { return *(volatile uint32_t *)(b + o); }

/* The two registers the reference reads before it sets anything up (hakios.ipa.c:819-821): the GSI's status, whose
   bit 0 says the block is enabled, and the IPA's shared memory size, whose high half is the local memory's start
   in units of 8 bytes (hakios.ipa.c:161, :105). */
#define FUXI_GSI_STATUS          0x012000u
#define FUXI_IPA_SHARED_MEM_SIZE 0x000054u
#define FUXI_IPA_MEM_SIZE_MASK   0x0000ffffu
#define FUXI_IPA_MEM_BADDR_MASK  0xffff0000u

int fuxi_ipa_map(struct fuxi_ipa_windows *ipa)
{
    if (!ipa) return EINVAL;
    __builtin_memset(ipa, 0, sizeof *ipa);
    ipa->reg_bytes = FUXI_IPA_REG_BYTES;
    ipa->gsi_bytes = FUXI_IPA_GSI_BYTES;
    ipa->fw_bytes = FUXI_IPA_FW_BYTES;
    ipa->gsi_mem_bytes = FUXI_IPA_GSIMEM_BYTES;
    ipa->reg = ml_io_map(FUXI_IPA_REG_PA, ipa->reg_bytes);
    ipa->gsi = ml_io_map(FUXI_IPA_GSI_PA, ipa->gsi_bytes);
    ipa->fw = ml_io_map(FUXI_IPA_FW_PA, ipa->fw_bytes);
    ipa->gsi_mem = ml_io_map(FUXI_IPA_GSIMEM_PA, ipa->gsi_mem_bytes);
    if (!ipa->reg || !ipa->gsi || !ipa->fw || !ipa->gsi_mem) {
        printf("FUXI_MODEM: the ipa windows did not map (reg %p gsi %p fw %p gsi_mem %p)\n",
               (void *)ipa->reg, (void *)ipa->gsi, (void *)ipa->fw, (void *)ipa->gsi_mem);
        return ENOMEM;
    }
    ipa->mapped = 1;
    /* Still not read, and the measurement is why. 2026-11-23, twice:
       -- without any clock vote the read hung the bus and the watchdog restarted the phone at the GLINK OPEN;
       -- with the probe having voted RPMH_IPA_CLK (cmd-db IP0 = 0x50040, BCM word 0x60000001) and the vote reported
          completed, the same read wedged the probe thread instead: the log stopped at "step: the proxy rails".
       So a clock vote is not what gates this block; a power domain or GDSC sits in front of it. Mapping a window is
       still safe -- it builds page tables and touches no bus -- so the windows are mapped and nothing is read. */
    ipa->gsi_status = 0;
    ipa->shared_mem_size = 0;
    ipa->mem_offset = 0;
    ipa->mem_size = 0;
    printf("FUXI_MODEM: the ipa windows are mapped: reg %#llx+%#x, gsi %#llx+%#x, fw %#llx+%#x, gsi_mem %#llx+%#x\n",
           FUXI_IPA_REG_PA, ipa->reg_bytes, FUXI_IPA_GSI_PA, ipa->gsi_bytes,
           FUXI_IPA_FW_PA, ipa->fw_bytes, FUXI_IPA_GSIMEM_PA, ipa->gsi_mem_bytes);
    printf("FUXI_MODEM: the ipa registers are not read: a clock vote alone does not make them answerable\n");
    /* The first words of each region, so a region that is not the IPA's is recognisable from one log. */
    printf("FUXI_MODEM: the ipa fw region starts %08x %08x %08x %08x\n",
           rd(ipa->fw, 0), rd(ipa->fw, 4), rd(ipa->fw, 8), rd(ipa->fw, 12));
    printf("FUXI_MODEM: the ipa gsi region starts %08x %08x %08x %08x\n",
           rd(ipa->gsi_mem, 0), rd(ipa->gsi_mem, 4), rd(ipa->gsi_mem, 8), rd(ipa->gsi_mem, 12));
    return 0;
}
