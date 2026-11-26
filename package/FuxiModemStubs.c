/* fuxi 2026-11-26, baseband port: what the ljhhkx529 modem bring-up links against and this tree does not have.
 *
 * The full CommCenter bridge (QcomBaseband/baseband/coachz_baseband.cpp) does not fit the iOS 17 iPhone fileset (it needs
 * about 110 KB of code and constants the native kernel has no room for; parked in .bb2-parked-20261126). In its place, the
 * adapter's bridge hooks were a small QMI UIM reader until stage 3, when the bridge itself fit: it asks the card's status, activates the USIM's primary GW session the
 * way ljhhkx529 found the modem accepts it (evidence 2026-11-26-uim-ready-operator.md: TLV 0x01 {u8 session_type 0, u8
 * activate 1}, TLV 0x10 {u8 slot 1, u8 aid_len, aid} with the USIM's own AID from the card status), and reads EF_ICCID and
 * EF_IMSI. The results go to the kernel log as FUXI_UIM lines. Nothing is published to CommCenter. */
#include <stdint.h>
#include <string.h>

extern int printf(const char *, ...);
extern uint64_t mach_absolute_time(void);

extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks) __attribute__((weak_import));
/* The UFS driver here has no readiness call: the partition table answering for modem_a is readiness. */
int native_ufs_part_ready(void)
{
    uint32_t lun = 0; uint64_t first = 0, blocks = 0;
    return native_ufs_part_find && native_ufs_part_find("modem_a", &lun, &first, &blocks);
}

