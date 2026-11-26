/* fuxi 2026-11-26: from ljhhkx529/QcomBaseband (fuxi_ipa.h), the modem GLINK/QRTR renamed modem_glink_/modem_qrtr_. */
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The IPA's windows on fuxi. Read only: this maps the register block and the two reserved regions the device
 * tree gives the IPA and reports what the hardware says about itself. It writes no register and loads no
 * firmware.
 *
 * The addresses are the device tree's (/soc/qcom,ipa@3e00000 in sm8550-xiaomi-fuxi.dtb):
 *   reg-names  "ipa-base" 0x03e00000 0x00084000
 *              "gsi-base" 0x03e04000 0x000fc000
 * and the two reserved regions the node's memory-regions and the ipa_fws PIL node name:
 *   /reserved-memory/ipa_fw_region@9b080000   0x9b080000 0x00010000
 *   /reserved-memory/ipa_gsi_region@9b090000  0x9b090000 0x0000a000
 *
 * CIPA_SHARED_MEM_SIZE is what makes the rest decidable: the IPA reports where its local memory starts and how
 * big it is, which is exactly what the INIT_DRIVER request has to carry (coachz_ipa_config.c:44-47 reads it the
 * same way). Until that value is read off this silicon the region the offsets are relative to is a guess, so
 * the report prints the number and the first words of both regions instead of assuming one.
 */
#ifndef FUXI_IPA_H
#define FUXI_IPA_H

#include <stdint.h>

#define FUXI_IPA_REG_PA   0x03e00000ull
#define FUXI_IPA_REG_BYTES 0x00084000u
#define FUXI_IPA_GSI_PA   0x03e04000ull
#define FUXI_IPA_GSI_BYTES 0x000fc000u
#define FUXI_IPA_FW_PA    0x9b080000ull
#define FUXI_IPA_FW_BYTES 0x00010000u
#define FUXI_IPA_GSIMEM_PA    0x9b090000ull
#define FUXI_IPA_GSIMEM_BYTES 0x0000a000u

struct fuxi_ipa_windows {
    vm_offset_t reg, gsi, fw, gsi_mem;
    uint32_t reg_bytes, gsi_bytes, fw_bytes, gsi_mem_bytes;
    /* What the hardware reports, in the reference's order (hakios.ipa.c:819-821). */
    uint32_t gsi_status, shared_mem_size, mem_offset, mem_size;
    uint8_t mapped;
};

/* Maps the four windows and reads the two registers. Returns 0 on success and leaves the struct readable. */
int fuxi_ipa_map(struct fuxi_ipa_windows *ipa);

#endif /* FUXI_IPA_H */
