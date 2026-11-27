/* fuxi 2026-11-26: from ljhhkx529/QcomBaseband fuxi 0b7f150 drivers/QcomBaseband/baseband/coachz_baseband.cpp. */
// SPDX-License-Identifier: BSD-2-Clause
// The cellular bridge: the IOKit side of Apple's PCIe baseband (AppleBaseband, AppleBasebandPCIControl and one
// AppleBasebandPCIInterface per channel, with the user-client classes the sandbox knows them by) over this SoC's modem. ABM and
// BBUpdater talk to these as to an MHI modem; the QMUX channels go to the modem's QMI services over QRTR (coachz_bb_qmux.h), the
// sahara channel is answered by a device end that reads and drops the firmware it is offered (coachz_bb_sahara.h), and the board
// methods are answered by coachz_bb_control.h. Started only with /chosen/coachz-baseband (tools/xnu_screen.py --baseband).
//
// Threads: the Wi-Fi thread owns QRTR (bsd/dev/coachz_wifi.c): it hands modem messages to coachz_baseband_qrtr and sends what
// coachz_baseband_poll gives it. User threads only queue. One lock covers the bridge state and the queues; user memory is copied
// outside it, and asynchronous reads complete on a thread call.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <kern/clock.h>

#include "coachz_bb_qmux.h"
#include "coachz_bb_sahara.h"
#include "coachz_bb_control.h"
#include "coachz_bb_channels.h"
#include "coachz_bb_data.h"
#include "coachz_bb_pdp.h"
#include "coachz_bb_esim.h"
#include "coachz_bb_ssctl.h"
#include "coachz_bb_uim_probe.h"
#include "coachz_bb_nas_probe.h"

#define BB_FRAME_MAX (64u * 1024u)       // one read or write: a QMUX frame, a SAHARA packet or a READ_DATA answer
// Frames waiting for ABM, per channel. At boot the 53 clients' registrations and indications arrive faster than ABM reads; with 64 the
// queue overflowed every boot (5 to 22 frames), a lost VOICE answer made CommCenter reset the baseband (ATCS_TIMEOUT) and lost UIM and
// NAS frames left the SIM and registration state stale until the SIM was reinserted. A dropped frame is a lost answer: keep room.
#define BB_QUEUE 1024u
#define BB_READS 8u                      // asynchronous reads waiting for a frame, per channel
#define BB_TX 1024u                      // QMI messages waiting for the Wi-Fi thread (a boot burst can be 400+ frames)
#define BB_SYNC_READ_SECONDS 30u

// Which SAHARA images the device end asks for. SBL1 (Qualcomm image id 0x15) is the first image a boot loader requests; the rest of
// a Mav16 boot (rpm, tz, apps, acdb, mba, qdsp6sw in the bbfw) follow once BBUpdater's log shows how it takes this one.
static const uint32_t bb_sahara_images[] = { 0x15 };

namespace {
struct bb_frame { uint32_t len; uint8_t *data; };
struct bb_read { OSAsyncReference64 reference; IOMemoryDescriptor *memory; uint32_t len; void *owner; };
struct bb_channel {
  bb_frame frames[BB_QUEUE];
  uint32_t head, count;
  bb_read reads[BB_READS];
  uint32_t reads_waiting;
  void *owner;
  uint32_t opens, frames_in, frames_out, frames_dropped, bytes_in, bytes_out;
};
struct bb_tx { uint32_t local_port, node, port, len; uint8_t *data; };
}

static IOLock *bb_lock;
static thread_call_t bb_complete_call;
static struct cbq_bridge *bb_qmux_heap;
#define bb_qmux (*bb_qmux_heap)
static struct cbs_device *bb_sahara_heap;
#define bb_sahara (*bb_sahara_heap)
static struct cbc_state bb_control;
static bb_channel (*bb_ch_heap)[CBH_PUBLISHED];
#define bb_ch (*bb_ch_heap)
static bb_tx (*bb_txq_heap)[BB_TX];
#define bb_txq (*bb_txq_heap)
static uint32_t bb_tx_head, bb_tx_count, bb_tx_dropped;
/* The refusals the link made since the last time they were reported. */
static uint32_t bb_tx_refused;
static uint32_t bb_refused_last;
/* Whether the last send went out. A send that the link refused (no room, or the peer is out of credit) has to be
   tried again, so the bridge asks. */
static int (*bb_send)(uint32_t local_port, uint32_t node, uint32_t port, const void *message, uint32_t length);
static void (*bb_del_client)(uint32_t local_port);
static int bb_published;
static uint32_t bb_private_seen[64], bb_private_count, bb_ctl_logged;
static uint8_t (*bb_out_heap)[BB_FRAME_MAX];     // answers built under the lock
#define bb_out (*bb_out_heap)
static struct cbp_client (*bb_pdp_clients_heap)[CBQ_MAX_CLIENTS];
#define bb_pdp_clients (*bb_pdp_clients_heap)
static uint32_t bb_pdp_owners[CBQ_MAX_CLIENTS];
static struct cbd_setup bb_data;         // the data port setup ModemManager runs for IPA (coachz_bb_data.h)
static int bb_data_slot[2] = { -1, -1 }; // the bridge's own DPM and WDA clients
static uint32_t bb_endpoint_fixes, bb_activations;
static uint8_t (*bb_fix_heap)[2048];
#define bb_fix (*bb_fix_heap)
static struct cbe *bb_esim_heap;
#define bb_esim (*bb_esim_heap)
static int bb_esim_slot = -1;
/* The eSIM branch is opt-in. The fuxi has a physical SIM in the slot and its modem has no VINYL service; the
   eSIM state machine then fails its route, and the branch's fallback answered the host's UIM requests with an
   internal error instead of the card's state. That is the wrong branch for this device: with coachz-esim unset
   the UIM traffic goes to the modem untouched, card status included. */
static int bb_esim_enabled;
static bb_tx (*bb_uim_held_heap)[BB_TX];
#define bb_uim_held (*bb_uim_held_heap)
static uint32_t bb_uim_head, bb_uim_count;
static uint8_t bb_uim_cid[BB_TX];
static uint8_t *bb_esim_in, *bb_esim_resp;
#define BB_ESIM_BYTES (1024u * 1024u)

// The modem's own list of its services, asked for over SSCTL (coachz_bb_ssctl.h). The name service does not
// answer this modem, so the list has to be asked of the modem itself.
static struct cbq_ssctl bb_ssctl;
static int bb_ssctl_slot = -1;

// The state of the SIM in the card slot, asked for over UIM (coachz_bb_uim_probe.h). The eSIM control only
// speaks when a client asks it to, and a physical card is not that: reading card slot one is one request.
static struct cbp_uim bb_uim;
static int bb_uim_slot = -1;
static int bb_session_tries;   /* session change attempts */
static int bb_session_at;      /* next attempt, ms */
static void bb_uim_session_try(const uint8_t *v10, uint32_t l10);
static int bb_uim_seq;          /* card power cycle state */
static int bb_uim_seq_at;       /* next step, ms */
static int bb_uim_poll_at;      /* next poll, ms */
static int bb_uim_poll_count;   /* polls sent */
static int bb_uim_ready;        /* an answer has said app 7 */
static void bb_queue_tx(uint32_t local_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t len);
static void bb_queue_tx(uint32_t local_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t len);

// The radio, asked for over DMS and NAS (coachz_bb_nas_probe.h). Three read-only requests: whether the radio is
// on, whether it has registered, and how strong the signal is. Nothing else asks them: the bridge forwards what
// CommCenter requests, and CommCenter's registration path runs through the Apple private services this modem does
// not have.
static struct cbn_probe bb_dms;
static int bb_dms_slot = -1;
static struct cbn_probe bb_serving;
static int bb_serving_slot = -1;
static struct cbn_probe bb_signal;
static int bb_radio_next;      /* next re-ask, ms */
static int bb_radio_tries;     /* how many times the signal/serving pair went out again */
static int bb_signal_slot = -1;
static struct cbn_radio bb_radio;

static uint32_t bb_now_ms(void)
{
  uint64_t ticks, ns;
  clock_get_uptime(&ticks);
  absolutetime_to_nanoseconds(ticks, &ns);
  return (uint32_t)(ns / 1000000u);
}

extern "C" {
int coachz_baseband_enabled(void);
void coachz_baseband_configure(int (*send)(uint32_t, uint32_t, uint32_t, const void *, uint32_t), void (*del_client)(uint32_t));
void coachz_baseband_new_server(uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port);
void coachz_baseband_del_server(uint32_t service, uint32_t node, uint32_t port);
int coachz_baseband_qrtr(uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body, uint32_t size);
void coachz_baseband_poll(void);
void coachz_baseband_report(unsigned second);
}

// ------------------------------------------------------------------ device tree
static OSData *bb_chosen(const char *path, const char *key)
{
  IORegistryEntry *entry = IORegistryEntry::fromPath(path, gIODTPlane);
  if (!entry) return nullptr;
  OSData *value = OSDynamicCast(OSData, entry->getProperty(key));
  if (value) value->retain();
  entry->release();
  return value;
}

extern "C" int coachz_baseband_enabled(void)
{
  static int cached = -1;
  if (cached < 0) {
    OSData *value = bb_chosen("/chosen", "coachz-baseband");
    uint32_t on = 0;
    if (value && value->getLength() == 4) memcpy(&on, value->getBytesNoCopy(), 4);
    if (!value) on = 1;   /* fuxi: the ADT has no such property; the bridge is on by default */
    OSSafeReleaseNULL(value);
    uint32_t arg = 0;
    if (PE_parse_boot_argn("coachz-baseband", &arg, sizeof arg)) on = arg;
    cached = on != 0;
  }
  return cached;
}

// ------------------------------------------------------------------ queues (bb_lock held)
static void bb_complete(thread_call_param_t, thread_call_param_t);

static bool bb_heap(void);
static void bb_init(void)
{
  if (bb_lock) return;
  if (!bb_heap()) {printf("COACHZ baseband: the bridge buffers were not allocated\n"); return;}
  bb_lock = IOLockAlloc();
  bb_complete_call = thread_call_allocate(bb_complete, nullptr);
  cbq_init(&bb_qmux);
  bb_esim_in = static_cast<uint8_t *>(IOMallocData(BB_ESIM_BYTES));
  bb_esim_resp = static_cast<uint8_t *>(IOMallocData(BB_ESIM_BYTES));
  cbe_init(&bb_esim, bb_esim_in, bb_esim_in ? BB_ESIM_BYTES : 0,
           bb_esim_resp, bb_esim_resp ? BB_ESIM_BYTES : 0);
  // Read-only eUICC answers (coachz_bb_esim.h): preserve 16 KiB per entry.
  const uint32_t cache_bytes = CBE_CACHE_ENTRIES * 16384u;
  uint8_t *cache = static_cast<uint8_t *>(IOMallocData(cache_bytes));
  cbe_set_cache(&bb_esim, cache, cache ? cache_bytes : 0);
  cbc_init(&bb_control);
  cbs_init(&bb_sahara, bb_sahara_images, sizeof bb_sahara_images / sizeof bb_sahara_images[0], 0x10000);
}

// Probe: QMI header fields of each message (service, type, message, transaction, length, QMI error), never TLV contents, so no APN,
// ICCID, APDU or address reaches the log. UIM, VINYL and WDS print every message; other services the first 6 of a kind, then each
// power of two. A budget keeps the log ring from filling; the periodic report says how many lines it held back.
#define BB_TRACE_KINDS 512u
#define BB_TRACE_BUDGET 6000u
static uint64_t (*bb_trace_key_heap)[BB_TRACE_KINDS];
#define bb_trace_key (*bb_trace_key_heap)
static uint32_t (*bb_trace_count_heap)[BB_TRACE_KINDS], bb_trace_lines, bb_trace_held;
#define bb_trace_count (*bb_trace_count_heap)
/* The CDC stream carries the console and the screenshot channel's replies together. In screenshot mode
   (sheng-fb-gpu=2) the raw dump of every QMI frame would starve those replies, so keep the per-kind summary
   and the periodic reports only. */
static bool bb_trace_quiet(void)
{
  static int quiet = -1;
  if (quiet < 0) {
    unsigned v = 0;
    quiet = (PE_parse_boot_argn("sheng-fb-gpu", &v, sizeof v) && v == 2) ? 1 : 0;
  }
  return quiet != 0;
}
static void bb_trace(const char *dir, uint32_t channel, uint32_t service, uint32_t cid, const uint8_t *sdu, uint32_t n)
{
  if (bb_trace_quiet()) return;
  if (!service || n < 7u) return;   // CTL has its own SDU layout and its own lines
  const uint32_t type = sdu[0], txn = cbd_rd16(sdu + 1), message = cbd_rd16(sdu + 3);
  /* Baseband probe: every message of the services that carry the card, the radio state and the activation, raw. */
  if (service == 0x2u || service == 0x3u || service == 0xbu || service == 0xe2u) {
    printf("COACHZ sdu: %s svc 0x%x cid %u type %u msg 0x%x txn %u len %u hex", dir, service, cid, type, message, txn, n);
    for (uint32_t i = 0; i < n && i < 160u; ++i) printf(" %02x", sdu[i]);
    printf("\n");
  }
  /* UIM card status indication: when the slot's USIM application is detected (state 1) but not ready (7),
     bind it to the primary GW session. Apple firmware does this itself; this modem leaves the application at
     DETECTED until the host asks (QMI UIM Change Provisioning Session 0x38). */
  if (service == 0xbu && message == 0x32u && type == 4u && bb_uim_seq >= 3) {
    uint32_t l10 = 0;
    const uint8_t *v10 = cbd_find(sdu, n, 0x10, &l10);
    if (v10) bb_uim_session_try(v10, l10);
  }
  /* DMS operating mode: the request's mode value and the answer's result/error, one line each and outside the
     dedup and the budget below. The mode is what says whether the upper layer asks for online (0) or low power
     (1); the answer's error says whether the modem took it. The direction string separates the bridge's own
     client (int) from ABM's (host/to-host). */
  if (service == 0x2u && (message == 0x2du || message == 0x2eu)) {
    if (type == 0u) {
      uint32_t mode_len = 0;
      const uint8_t *mode = message == 0x2eu ? cbd_find(sdu, n, 0x01, &mode_len) : nullptr;
      printf("COACHZ dms: %s cid %u msg 0x%x request mode %d t %u\n", dir, cid, message,
             mode && mode_len >= 1u ? (int)mode[0] : -1, bb_now_ms());
    } else if (type == 2u) {
      uint32_t result = 0xffffu, error = 0xffffu, rlen = 0;
      if (cbd_sdu_ok(sdu, n)) {
        const uint8_t *r = cbd_find(sdu, n, 0x02, &rlen);
        if (r && rlen >= 4u) { result = cbd_rd16(r); error = cbd_rd16(r + 2); }
      }
      printf("COACHZ dms: %s cid %u msg 0x%x answer result %u err 0x%x t %u\n", dir, cid, message, result, error, bb_now_ms());
    }
  }
  const uint32_t error = type == 2u ? (cbd_sdu_ok(sdu, n) ? cbd_error(sdu, n) : 0xfffeu) : 0u;
  const uint64_t key = 1u + ((uint64_t)((uint32_t)dir[0] << 8 | (uint32_t)dir[4]) << 48 |(uint64_t)service << 40 | (uint64_t)type << 32 |
    (uint64_t)message << 16 | error);
  uint32_t count = 0;
  for (uint32_t i = 0, at = (uint32_t)(key * 0x9e3779b97f4a7c15ull >> 55) % BB_TRACE_KINDS; i < BB_TRACE_KINDS; i++, at = (at + 1) % BB_TRACE_KINDS) {
    if (bb_trace_key[at] != key && bb_trace_key[at]) continue;
    bb_trace_key[at] = key;
    count = ++bb_trace_count[at];
    break;
  }
  const bool always = service == CBE_SVC_UIM || service == CBE_SVC_VINYL || service == CBD_SVC_WDS;
  if (!always && count > 6u && (count & (count - 1u))) return;
  if (bb_trace_lines >= BB_TRACE_BUDGET) { bb_trace_held++; return; }
  bb_trace_lines++;
  printf("COACHZ qmi: %s ch %u svc 0x%x cid %u type %u msg 0x%x txn %u len %u err 0x%x n %u t %u\n",
         dir, channel, service, cid, type, message, txn, n, error, count, bb_now_ms());
}
/* Service 0 (CTL) travels in two different header layouts: cbq_ctl reads the host side as flags, txn, message
   id (2..3), TLV length (4..5), while cbq_from_modem reads the modem side with the generic QMI layout (length at
   5..6). The raw bytes and the two fields below settle which layout the modem actually uses. */
static void bb_ctl_note(const char *dir, uint32_t channel, const uint8_t *s, uint32_t n)
{
  char hex[3u * 16u + 1u];
  const uint32_t take = n < 16u ? n : 16u;
  for (uint32_t i = 0; i < take; i++) {
    static const char digits[] = "0123456789abcdef";
    hex[i * 3u + 0u] = digits[s[i] >> 4];
    hex[i * 3u + 1u] = digits[s[i] & 15u];
    hex[i * 3u + 2u] = ' ';
  }
  hex[take * 3u] = '\0';
  printf("COACHZ ctl: %s ch %u txn %u msg 0x%x tlvlen %u len %u hex %s\n", dir, channel,
         n >= 2u ? (uint32_t)s[1] : 0u, n >= 4u ? (uint32_t)(s[2] | s[3] << 8) : 0xffffu,
         n >= 6u ? (uint32_t)(s[4] | s[5] << 8) : 0xffffu, n, hex);
}

// A QMUX frame (I/F type 1, length, flags, service, client id, then the SDU).
static void bb_trace_frame(const char *dir, uint32_t channel, const uint8_t *frame, uint32_t len)
{
  if (len >= CBQ_HDR + 2u && frame[0] == 1u && frame[4] == 0u) bb_ctl_note(dir, channel, frame + CBQ_HDR, len - CBQ_HDR);
  if (len >= CBQ_HDR + 7u && frame[0] == 1u) bb_trace(dir, channel, frame[4], frame[5], frame + CBQ_HDR, len - CBQ_HDR);
}

static void bb_push(uint32_t channel, const uint8_t *data, uint32_t len)
{
  if (channel == CBH_QMUX1 || channel == CBH_QMUX2) bb_trace_frame("to-host", channel == CBH_QMUX1 ? 1u : 2u, data, len);
  bb_channel *c = &bb_ch[channel];
  uint8_t *copy = len && len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
  if (!copy || c->count == BB_QUEUE) {
    if (copy) IOFreeData(copy, len);
    // Probe: which answer was lost (header fields only).
    if (c->frames_dropped < 32u)
      printf("COACHZ baseband: dropped frame for ABM on channel %u (%s, %u queued, %u bytes) svc 0x%x type %u msg 0x%x\n", channel,
             copy ? "queue full" : "no memory", c->count, len, len > 4 ? data[4] : 0u, len > CBQ_HDR ? data[CBQ_HDR] : 0u,
             len > CBQ_HDR + 4 ? (uint32_t)(data[CBQ_HDR + 3] | data[CBQ_HDR + 4] << 8) : 0u);
    c->frames_dropped++;
    return;
  }
  memcpy(copy, data, len);
  c->frames[(c->head + c->count++) % BB_QUEUE] = { len, copy };
  c->frames_out++;
  c->bytes_out += len;
  IOLockWakeup(bb_lock, c, false);
  if (c->reads_waiting && bb_complete_call) thread_call_enter(bb_complete_call);
}

static bb_frame bb_pop(uint32_t channel)
{
  bb_channel *c = &bb_ch[channel];
  bb_frame f = { 0, nullptr };
  if (!c->count) return f;
  f = c->frames[c->head];
  c->head = (c->head + 1) % BB_QUEUE;
  c->count--;
  return f;
}

static void bb_flush(uint32_t channel)
{
  for (bb_frame f = bb_pop(channel); f.data; f = bb_pop(channel)) IOFreeData(f.data, f.len);
}

/* Where the bridge's own transmissions are lost. The queue holds 64 frames, the poll drains at most 16 per pass and
   puts a refused send back only when there is room, so a busy pass can lose frames in two places. One line per
   (service, message) at the first drop and at each power of two keeps the Wi-Fi thread from printing a flood while
   still naming the traffic that goes missing: the DMS mode request is the one the upper layer notices as a baseband
   that never comes online. */
static void bb_tx_drop_note(const char *where, uint32_t local_port, const uint8_t *data, uint32_t len)
{
  struct { uint32_t key, count; } static seen[32];
  const uint32_t slot = local_port >= CBQ_PORT_BASE ? local_port - CBQ_PORT_BASE : ~0u;
  const uint32_t svc = slot < CBQ_MAX_CLIENTS && bb_qmux.client[slot].used ? bb_qmux.client[slot].service : 0xffffu;
  /* CTL (service 0) packs its message id at 2..3 (flags, transaction, id, TLV length); every other QMI service
     at 3..4 (type, transaction, id, length). Reading the generic offset for CTL reports msg_hi | tlv_lo, which names
     message ids that do not exist. */
  const uint32_t msg = svc == 0u ? (len >= 6u ? (uint32_t)data[2] | (uint32_t)data[3] << 8 : 0xffffu)
                                 : (len >= 7u ? (uint32_t)data[3] | (uint32_t)data[4] << 8 : 0xffffu);
  const uint32_t key = (svc << 16) | msg;
  bb_tx_dropped++;
  for (uint32_t i = 0; i < 32u; i++) {
    if (seen[i].key != key && seen[i].key) continue;
    seen[i].key = key;
    const uint32_t n = ++seen[i].count;
    if (n == 1u || (n & (n - 1u)) == 0u)
      printf("COACHZ txdrop: %s svc 0x%x msg 0x%x (%u, total %u)\n", where, svc, msg, n, bb_tx_dropped);
    return;
  }
}

/* One line per stage of a DMS Set Operating Mode (service 0x02, message 0x2e): the request the upper layer
   sends, whether the poll sends it or the link refuses it, whether it goes back on the queue, and whether the
   modem answers it. The same cid and transaction identify the four lines of one attempt. */
static void bb_dms2e_note(const char *stage, uint32_t local_port, const uint8_t *sdu, uint32_t n, int rc)
{
  const uint32_t slot = local_port >= CBQ_PORT_BASE ? local_port - CBQ_PORT_BASE : ~0u;
  const bool used = slot < CBQ_MAX_CLIENTS && bb_qmux.client[slot].used;
  const uint32_t svc = used ? bb_qmux.client[slot].service : 0xffffu;
  const uint32_t msg = n >= 7u ? (uint32_t)(sdu[3] | sdu[4] << 8) : 0xffffu;
  if (svc != 0x2u || (msg != 0x2du && msg != 0x2eu)) return;
  /* One request is identified by its client id and transaction; the refusal loop of run-20261125-081201 printed
     the same pair a million times. The first two lines and each power of two keep the count visible without the
     flood, and the counter itself says how often the stage ran. */
  struct { uint32_t key, count; } static seen[16];
  const uint32_t cid = used ? (uint32_t)bb_qmux.client[slot].cid : 0xffffu;
  const uint32_t txn = n >= 7u ? (uint32_t)(sdu[1] | sdu[2] << 8) : 0xffffu;
  const uint32_t key = cid * 0x10000u + (txn & 0xffffu);
  uint32_t count = 0;
  for (uint32_t i = 0; i < 16u; i++) {
    if (seen[i].key != key && seen[i].key) continue;
    seen[i].key = key;
    count = ++seen[i].count;
    break;
  }
  if (count > 2u && (count & (count - 1u))) return;
  /* The channel says whether the request came from the bridge (internal) or from ABM; the full SDU is printed on
     the lines that survive the limit, which is what tells an identical request from one a step apart. */
  char hex[3u * 32u + 1u];
  const uint32_t take = n < 32u ? n : 32u;
  for (uint32_t i = 0; i < take; i++) {
    static const char digits[] = "0123456789abcdef";
    hex[i * 3u + 0u] = digits[sdu[i] >> 4];
    hex[i * 3u + 1u] = digits[sdu[i] & 15u];
    hex[i * 3u + 2u] = ' ';
  }
  hex[take * 3u] = '\0';
  printf("COACHZ dmsmode: %s ch %u svc 0x2 cid %u txn %u msg 0x%x len %u rc %d queue %u count %u hex %s t %u\n",
         stage, used ? (unsigned)bb_qmux.client[slot].channel : 0xffffu, cid, txn, msg, n, rc, bb_tx_count,
         count, hex, bb_now_ms());
}

// A QMI message for the modem, sent by the Wi-Fi thread from coachz_baseband_poll.
static void bb_queue_tx(uint32_t local_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t len)
{
  /* Diagnostic: the first frames the host sends to NAS, so the log says whether CommCenter drives the NAS
     client it created. A host that never sends one cannot be shown the registration any other way. */
  {
    static uint32_t seen;
    const uint32_t slot = local_port >= CBQ_PORT_BASE ? local_port - CBQ_PORT_BASE : ~0u;
    if (seen < 12u && slot < CBQ_MAX_CLIENTS && bb_qmux.client[slot].used &&
        bb_qmux.client[slot].service == CBN_SVC_NAS && !bb_qmux.client[slot].private_service) {
      const uint32_t m = len >= 7u ? (uint32_t)(data[3] | data[4] << 8) : 0xffffu;
      seen++;
      printf("COACHZ nas-host: msg 0x%x cid %u len %u channel %u (%u)\n", m, bb_qmux.client[slot].cid, len,
             bb_qmux.client[slot].channel, seen);
    }
  }
  uint8_t *copy = len ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
  /* >= rather than ==: a count that has ever passed the limit must keep refusing instead of wrapping the ring
     and overwriting the frames already queued. */
  if (!copy || bb_tx_count >= BB_TX) {
    if (copy) IOFreeData(copy, len);
    bb_dms2e_note("enq-drop", local_port, data, len, -1);
    bb_tx_drop_note(bb_tx_count >= BB_TX ? "queue full" : "no memory", local_port, data, len);
    return;
  }
  memcpy(copy, data, len);
  bb_txq[(bb_tx_head + bb_tx_count++) % BB_TX] = { local_port, node, port, len, copy };
  bb_dms2e_note("enqueue", local_port, data, len, 0);
  const uint32_t slot = local_port - CBQ_PORT_BASE;
  if (slot < CBQ_MAX_CLIENTS && bb_qmux.client[slot].used && bb_qmux.client[slot].channel == CBQ_INTERNAL) {
    bb_trace("int-tx", 0, bb_qmux.client[slot].service, bb_qmux.client[slot].cid, data, len);
  } else if (slot < CBQ_MAX_CLIENTS && bb_qmux.client[slot].used) {
    /* The fourth state is a request from the upper layer's own QMI client (the QMUX channel), not one of the
       bridge's internal probes: those are traced above and reach the modem whatever CommCenter is doing. */
    static bool stage4_logged;
    if (!stage4_logged) {
      stage4_logged = true;
      printf("FUXI_STAGE 4/4 commcenter: the first QMI request from the upper layer reached the modem (service 0x%02x)\n",
             bb_qmux.client[slot].service);
    }
  }
}

// BBUEUR13Loader pushes the SBL with SendImage on pbl, frees that transport, opens sahara and waits 40 s for the loaded SBL's HELLO there. So the
// HELLO goes out when sahara opens after a SendImage.
static bool bb_boot_image_sent;

// ------------------------------------------------------------------ operating mode after a boot (bb_lock held)
// ABM powers the baseband up at every start and the bridge stands for that boot (bb_sahara_restart) while the
// modem keeps running, so the modem keeps whatever mode it had. A baseband that ABM has just booted comes up in
// low power: CommCenter logs "Baseband booted in airplane mode", and its subscription switch waits for
// /cc/props/radio_state to reach Offline, which only the modem's low-power report gives. On the reference board
// the modem never left the mode CommCenter reads as Shutdown, so the switch never went on and the SIM stayed
// unknown, with no service. After each such boot the bridge's own DMS client reads the mode and puts the modem
// in low power (QMI DMS Get/Set Operating Mode 0x002d/0x002e, TLV 0x01 u8: 0 online, 1 low power); CommCenter
// then takes it online itself. Layouts are libqmi's (qmi-service-dms).
static const uint32_t BB_DMS = 0x02u, BB_DMS_GET_MODE = 0x002du, BB_DMS_SET_MODE = 0x002eu;
static uint8_t bb_lpm_pending, bb_lpm_step;
/* Without ABM nothing opens the sahara channel, so the handshake above would never run and its QMI could not be
   checked. This runs it once, on the bridge's own first DMS answer, when the boot argument asks for it. */
static uint8_t bb_lpm_once, bb_lpm_tried;
static uint32_t bb_lpm_txn = 0x4c00;
static void bb_lpm_send(void)
{
  const struct cbq_server *server = cbq_server_find(&bb_qmux, BB_DMS);
  if (!server) { if (bb_lpm_step == 0) bb_lpm_pending = 1; return; }   // DMS not announced yet: try again on the next frame
  if (bb_dms_slot < 0) bb_dms_slot = cbq_client_alloc(&bb_qmux, CBQ_INTERNAL, BB_DMS);
  if (bb_dms_slot < 0) { printf("COACHZ baseband: operating mode: no DMS client\n"); bb_lpm_pending = 0; return; }
  uint8_t sdu[16];
  uint32_t n;
  bb_lpm_txn++;
  if (bb_lpm_step == 0) {
    n = cbd_request(sdu, bb_lpm_txn, BB_DMS_GET_MODE, 0);
  } else {
    const uint8_t low_power = 1;
    n = cbd_request(sdu, bb_lpm_txn, BB_DMS_SET_MODE, cbd_tlv(sdu + 7, sizeof sdu - 7u, 0x01, &low_power, 1));
  }
  bb_queue_tx(CBQ_PORT_BASE + (uint32_t)bb_dms_slot, server->node, server->port, sdu, n);
}
// An answer on the bridge's DMS client: 1 when it was one.
static int bb_lpm_answer(uint32_t dst_port, const uint8_t *sdu, uint32_t n)
{
  if (bb_dms_slot < 0 || dst_port != CBQ_PORT_BASE + (uint32_t)bb_dms_slot) return 0;
  /* Not our transaction: this client is shared with the radio probe (bb_radio_step), which asks its own question on
     the same DMS client, so an answer that is not ours has to fall through to that probe. The reference has this
     client to itself and may swallow anything, which is why its return value here cannot be copied as it stands. */
  if (n < 7u || sdu[0] != 0x02 || cbd_rd16(sdu + 1) != bb_lpm_txn) return 0;
  const uint32_t error = cbd_error(sdu, n);
  if (bb_lpm_step == 0) {
    uint32_t len = 0;
    const uint8_t *mode = cbd_find(sdu, n, 0x01, &len);
    printf("COACHZ baseband: operating mode after the boot: %d (error 0x%x), setting low power\n",
           mode && len >= 1 ? (int)mode[0] : -1, error);
    bb_lpm_step = 1;
    bb_lpm_send();
  } else {
    printf("COACHZ baseband: operating mode set to low power: error 0x%x\n", error);
    bb_lpm_pending = 0;
  }
  return 1;
}
static void bb_sahara_restart(void)
{
  struct cbs_out o;
  /* The only caller is the sahara channel's open with a boot image sent, so this line answers one question:
     does the upper layer re-open the channel right after the last DONE_RESP? The state and the image index
     below are the ones the state machine had at that moment, before cbs_init resets them. */
  printf("COACHZ baseband: sahara restart on channel open: state %u, image %u of %u, last command %u, %u requests\n",
         bb_sahara.state, bb_sahara.image_index + 1u, bb_sahara.image_count, bb_sahara.last_command, bb_sahara.requests);
  bb_flush(CBH_SAHARA);
  cbs_init(&bb_sahara, bb_sahara_images, sizeof bb_sahara_images / sizeof bb_sahara_images[0], 0x10000);
  cbs_start(&bb_sahara, &o);
  for (uint32_t i = 0; i < o.count; i++) bb_push(CBH_SAHARA, o.data[i], o.len[i]);
  /* The low-power handshake belongs to the modem's own boot: the first sahara channel open after the modem
     starts. Every later open is ABM re-opening the channel for another boot attempt, and repeating the
     handshake there set the modem back to low power after ABM had already taken it online (DMS timeline:
     online result 0 at t=80319, the bridge's mode 1 again at t=106344). */
  static bool lpm_armed_once;
  if (!lpm_armed_once) {
    lpm_armed_once = true;
    bb_lpm_pending = 1;
    bb_lpm_step = 0;
  }
  printf("COACHZ baseband: sahara hello\n");
}

static void bb_private_note(uint32_t service, uint32_t message)
{
  const uint32_t key = service << 16 | (message & 0xffff);
  for (uint32_t i = 0; i < bb_private_count; i++)
    if (bb_private_seen[i] == key) return;
  if (bb_private_count < 64) bb_private_seen[bb_private_count++] = key;
  printf("COACHZ baseband: private QMI service 0x%x message 0x%x answered not supported\n", service, message);
}

// ------------------------------------------------------------------ data port setup (bb_lock held)
static void bb_data_step(void)
{
  uint8_t sdu[128];
  uint32_t service = 0;
  const uint32_t n = cbd_next(&bb_data, sdu, sizeof sdu, &service);
  if (!n) {
    printf("COACHZ baseband: data port: DPM open %s 0x%x; WDA set data format error 0x%x, link %d, uplink %d, downlink %d (asked %u), %d datagrams, %d bytes\n",
           bb_data.has_dpm ? "error" : "skipped, no service,", bb_data.dpm_error, bb_data.wda_error, (int)bb_data.llp, (int)bb_data.ul_dap,
           (int)bb_data.dl_dap, cbd_daps[bb_data.dap_index], (int)bb_data.dl_datagrams, (int)bb_data.dl_size);
    return;
  }
  int *slot = &bb_data_slot[service == CBD_SVC_DPM ? 0 : 1];
  if (*slot < 0) *slot = cbq_client_alloc(&bb_qmux, CBQ_INTERNAL, service);
  const struct cbq_server *server = cbq_server_find(&bb_qmux, service);
  if (*slot < 0 || !server) {
    printf("COACHZ baseband: data port: no client for service %u\n", service);
    bb_data.step = CBD_DONE;
    return;
  }
  bb_queue_tx(CBQ_PORT_BASE + (uint32_t)*slot, server->node, server->port, sdu, n);
}

static void bb_data_start(void)
{
  if (bb_data.step != CBD_IDLE) return;
  bb_data.has_dpm = cbq_server_find(&bb_qmux, CBD_SVC_DPM) != nullptr;
  bb_data.has_wda = cbq_server_find(&bb_qmux, CBD_SVC_WDA) != nullptr;
  bb_data_step();
}

// WDS is still entirely owned by CommCenter. Observe only wire lifecycle facts, never credentials or addresses.
static void bb_pdp_note(uint32_t local_port, uint32_t cid, uint32_t channel, const uint8_t *sdu, uint32_t n)
{
  const uint32_t slot = local_port - CBQ_PORT_BASE;
  if (slot >= CBQ_MAX_CLIENTS || !cbd_sdu_ok(sdu, n)) return;
  const uint32_t owner = 1u + cid + (channel << 8);
  if (bb_pdp_owners[slot] != owner) {
    bzero(&bb_pdp_clients[slot], sizeof bb_pdp_clients[slot]);
    bb_pdp_clients[slot].family = 4; // WDS default family, overridden by Set IP Family or Start Network
    bb_pdp_owners[slot] = owner;
  }
  cbp_client &c = bb_pdp_clients[slot];
  if (cbp_observe(&c, sdu, n))
    printf("COACHZ pdp: WDS type %u message 0x%x txn %u mux %u family %u active %u error %u address-fields %u\n",
      sdu[0], cbd_rd16(sdu + 3), cbd_rd16(sdu + 1), c.mux, c.family, c.active,
      sdu[0] == 2 ? cbd_error(sdu, n) : 0, c.settings);
  const bool start_failed = sdu[0] == 2 && cbd_rd16(sdu + 3) == CBP_START && cbd_error(sdu, n);
  if (start_failed || (sdu[0] == 4 && cbd_rd16(sdu + 3) == CBP_STATUS && !c.active)) {
    // libqmi WDS 0x20 responses and 0x22 indications use the same call-end TLVs.
    uint32_t reason_size = 0, verbose_size = 0;
    const uint8_t *reason = cbd_find(sdu, n, 0x10, &reason_size);
    const uint8_t *verbose = cbd_find(sdu, n, 0x11, &verbose_size);
    printf("COACHZ pdp: %s cid %u channel %u mux %u family %u reason-present %u reason %u verbose-present %u type %u code %d\n",
      start_failed ? "start failed" : "disconnected", cid, channel, c.mux, c.family, reason && reason_size == 2u,
      reason && reason_size == 2u ? cbd_rd16(reason) : 0u, verbose && verbose_size == 4u,
      verbose && verbose_size == 4u ? cbd_rd16(verbose) : 0u,
      verbose && verbose_size == 4u ? static_cast<int16_t>(cbd_rd16(verbose + 2)) : 0);
  }
}

static void bb_sim_request(const struct cbq_result &r, const uint8_t *sdu);

// A request for the modem, with the data port named as this modem has it.
static void bb_to_modem(const struct cbq_result &r, const uint8_t *sdu)
{
  if (r.service == CBD_SVC_WDS) bb_pdp_note(r.local_port, r.cid, r.channel, sdu, r.len);
  bb_sim_request(r, sdu);
  if (bb_esim_enabled && r.service == CBE_SVC_UIM && bb_esim.route_failed) {
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, CBE_ERR_INTERNAL, nullptr, 0, bb_out, sizeof bb_out);
    if (n) bb_push(r.channel ? CBH_QMUX2 : CBH_QMUX1, bb_out, n);
    return;
  }
  if (bb_esim_enabled && r.service == CBE_SVC_UIM && bb_esim_in && bb_esim_resp &&
      (bb_esim.freeze || (!bb_esim.slots.known && r.len >= 7 && cbd_rd16(sdu + 3) == CBE_UIM_GET_CARD_STATUS))) {
    uint8_t *copy = bb_uim_count < BB_TX ? static_cast<uint8_t *>(IOMallocData(r.len)) : nullptr;
    if (copy) {
      memcpy(copy, sdu, r.len);
      const uint32_t slot = (bb_uim_head + bb_uim_count++) % BB_TX;
      bb_uim_held[slot] = { r.local_port, r.node, r.port, r.len, copy };
      bb_uim_cid[slot] = r.cid;
    } else {
      const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, CBE_ERR_DEVICE_IN_USE, nullptr, 0, bb_out, sizeof bb_out);
      if (n) bb_push(r.channel ? CBH_QMUX2 : CBH_QMUX1, bb_out, n);
    }
    return;
  }
  const struct cbd_fix f = cbd_endpoint_fix(r.service, sdu, r.len, bb_fix, sizeof bb_fix);
  if (!f.len) {
    bb_queue_tx(r.local_port, r.node, r.port, sdu, r.len);
    return;
  }
  if (bb_endpoint_fixes++ < 8)
    printf("COACHZ baseband: data port: service %u message 0x%x endpoint %u/%u %s %u/%u\n", r.service, (uint32_t)(sdu[3] | sdu[4] << 8),
           f.before_type, f.before_interface, f.inserted ? "added as" : "now", CBD_EP_EMBEDDED, CBD_EP_INTERFACE);
  bb_queue_tx(r.local_port, r.node, r.port, bb_fix, f.len);
}

// The modem's answer about itself, on the record.
static void bb_ssctl_log(const uint8_t *sdu, uint32_t size)
{
  printf("COACHZ baseband: the SSCTL answer: error %u, %u bytes of reason\n", bb_ssctl.error, bb_ssctl.reason_len);
  if (bb_ssctl.reason_len) printf("COACHZ baseband: the modem says: %s\n", bb_ssctl.reason);
  /* The answer's own bytes. The TLV layout of the reason is not in this tree, so the bytes are what settles
     it: an answer that names a reason and one that does not look different in a way that can be read off the
     log. */
  printf("COACHZ baseband: the answer is %u bytes:", size);
  for (uint32_t i = 0; i < size && i < 64; ++i) printf(" %02x", sdu[i]);
  printf("\n");
  printf("COACHZ baseband: the answer carries %u TLVs:", bb_ssctl.tlv_count);
  for (uint32_t i = 0; i < bb_ssctl.tlv_count; ++i)
    printf(" type 0x%02x of %u bytes", bb_ssctl.tlv_types[i], bb_ssctl.tlv_lens[i]);
  printf("\n");
}

// The one SSCTL request that is safe to send: the failure reason.
//
// The SSCTL service is subsystem control, and two of its four messages stop the modem: 0x0020 restarts the
// modem's subsystem and 0x0021 shuts it down. The earlier sweep of the message ids was asking for exactly
// those, one id a second, and the modem stopped answering because it had been told to stop. The ids 0x0020
// and 0x0021 are never sent from here.
//
// What is asked instead is id 0x0022, the failure reason, which takes no TLVs and only reads. It is asked
// once, and only after the modem has been quiet for a while, because a modem that is running has no failure
// to report and the answer would be empty. The reason is the modem's own statement of why it stopped, which
// is the one thing the log cannot work out on its own.
static void bb_ssctl_step(void)
{
  const struct cbq_server *server = cbq_server_find(&bb_qmux, CBS_SVC_SSCTL);
  if (!server) return;
  if (bb_ssctl_slot < 0) bb_ssctl_slot = cbq_client_alloc(&bb_qmux, CBQ_INTERNAL, CBS_SVC_SSCTL);
  if (bb_ssctl_slot < 0) return;
  if (bb_ssctl.sent || bb_ssctl.answered) return;
  uint8_t sdu[32];
  const uint32_t n = cbs_build_request_id(sdu, sizeof sdu, 0x22u, CBS_SSCTL_GET_FAILURE_REASON);
  if (!n) return;
  const uint32_t queued = bb_tx_count;
  bb_queue_tx(CBQ_PORT_BASE + (uint32_t)bb_ssctl_slot, server->node, server->port, sdu, n);
  /* The request is asked once. A queue that was full is asked again on the next poll, and a request that did
     go out is not asked again: the failure reason does not change while the modem runs. */
  if (bb_tx_count != queued) bb_ssctl.sent = 1;
}

// One request to the modem's UIM service: the status of the card slots (coachz_bb_uim_probe.h).
//
// The request asks for the state of the card and nothing else. It does not switch the slot, it does not
// authenticate and it writes nothing to the card, so it is safe to send once when the modem has published UIM.
//
// The request is asked once. A queue that was full is asked again on the next poll, and a request that did go
// out is not asked again: the card's state does not change while the modem runs.
/* Power Off, settle, Power On, settle, then the asks. The USIM application has been reported at state 1
   (detected) on every boot, and the one earlier run that sent 0x30 alone made the card absent, which is
   Power Off's own result: the card's power state was never driven on after its initial bring-up. The
   sequence is a fresh card power cycle on the bridge's own client; nothing else in the card is written. */
static void bb_uim_step(void)
{
  const struct cbq_server *server = cbq_server_find(&bb_qmux, CBP_SVC_UIM);
  if (!server) return;
  if (bb_uim_slot < 0) bb_uim_slot = cbq_client_alloc(&bb_qmux, CBQ_INTERNAL, CBP_SVC_UIM);
  if (bb_uim_slot < 0) return;
  if (bb_uim_seq >= 3) return;
  const uint32_t now = bb_now_ms();
  if (bb_uim_seq_at && now < (uint32_t)bb_uim_seq_at) return;
  uint8_t sdu[32];
  const uint32_t port = CBQ_PORT_BASE + (uint32_t)bb_uim_slot;
  const uint32_t queued = bb_tx_count;
  static const uint8_t slot_one[] = { 0x01u };
  uint32_t n;
  if (bb_uim_seq == 0) {
    if ((n = cbp_build_tlv(sdu, sizeof sdu, 0x44u, CBP_UIM_POWER_OFF, 0x01u, slot_one, 1u)) != 0u)
      bb_queue_tx(port, server->node, server->port, sdu, n);
    bb_uim_seq = 1; bb_uim_seq_at = (int)(now + 800u);
  } else if (bb_uim_seq == 1) {
    if ((n = cbp_build_tlv(sdu, sizeof sdu, 0x45u, CBP_UIM_POWER_ON, 0x01u, slot_one, 1u)) != 0u)
      bb_queue_tx(port, server->node, server->port, sdu, n);
    bb_uim_seq = 2; bb_uim_seq_at = (int)(now + 800u);
  } else {
    if ((n = cbp_build_register(sdu, sizeof sdu, 0x40u)) != 0u)
      bb_queue_tx(port, server->node, server->port, sdu, n);
    if ((n = cbp_build_request(sdu, sizeof sdu, 0x41u, 0x2fu)) != 0u)
      bb_queue_tx(port, server->node, server->port, sdu, n);
    if ((n = cbp_build_request(sdu, sizeof sdu, 0x42u, CBP_UIM_GET_SLOT_STATUS)) != 0u)
      bb_queue_tx(port, server->node, server->port, sdu, n);
    bb_uim_seq = 3;
  }
  if (bb_tx_count != queued) bb_uim.sent = 1;
}

/* UMi's UIM sequence (umi_modem_mhi.inc, md_stage "QMI-UIM"): ask Get Card Status again every couple of seconds
   until the USIM application reports 7 (ready), because the application moves to ready some time after the slot
   is detected. The bridge's first ask can come before that, so one ask is not enough. Read-only. */
static void bb_uim_poll_step(void)
{
  if (bb_uim_ready) return;
  if (bb_uim_poll_count >= 90) return;                       /* 180 s cap */
  const struct cbq_server *server = cbq_server_find(&bb_qmux, CBP_SVC_UIM);
  if (!server) return;
  if (bb_uim_slot < 0) bb_uim_slot = cbq_client_alloc(&bb_qmux, CBQ_INTERNAL, CBP_SVC_UIM);
  if (bb_uim_slot < 0) return;
  const uint32_t now = bb_now_ms();
  if (bb_uim_poll_at && now < (uint32_t)bb_uim_poll_at) return;
  bb_uim_poll_at = (int)(now + 2000u);
  uint8_t sdu[32];
  const uint32_t n = cbp_build_request(sdu, sizeof sdu, 0x60u + (uint32_t)bb_uim_poll_count, 0x2fu);
  if (!n) return;
  const uint32_t queued = bb_tx_count;
  bb_queue_tx(CBQ_PORT_BASE + (uint32_t)bb_uim_slot, server->node, server->port, sdu, n);
  if (bb_tx_count != queued) bb_uim_poll_count++;
}

/* Ask the modem to bind the slot's USIM application to the primary GW provisioning session, with the AID the
   card itself reports. libqmi's Change Provisioning Session: TLV 0x01 { u8 session_type, u8 activate } and
   TLV 0x10 { u8 slot, u8 aid_len, u8 aid[] }. Up to three attempts, 15 s apart. */
static void bb_uim_session_try(const uint8_t *v10, uint32_t l10)
{
  if (bb_uim_slot < 0 || bb_uim_seq < 3 || bb_session_tries >= 3) return;
  const uint32_t now = bb_now_ms();
  if (bb_session_at && now < (uint32_t)bb_session_at) return;
  uint32_t cards = 0, app_state = 0, aid_len = 0;
  uint8_t aid[32];
  if (!cbp_card_usim_aid(v10, l10, &cards, &app_state, aid, &aid_len) || app_state == 7u || !aid_len) return;
  const struct cbq_server *srv = cbq_server_find(&bb_qmux, CBE_SVC_UIM);
  if (!srv) return;
  uint8_t rsdu[64];
  const uint32_t m = cbp_build_session(rsdu, sizeof rsdu, 0x50u + (uint32_t)bb_session_tries,
                                       CBP_UIM_SESSION_PRIMARY_GW, 1u, 1u, aid, (uint8_t)aid_len);
  if (!m) return;
  bb_queue_tx(CBQ_PORT_BASE + (uint32_t)bb_uim_slot, srv->node, srv->port, rsdu, m);
  ++bb_session_tries;
  bb_session_at = (int)(now + 15000u);
  printf("COACHZ uim: session change %d sent, app_state %u, aid %u bytes, t %u\n",
         bb_session_tries, app_state, aid_len, now);
}

// The modem's answer about the card, on the record.
static void bb_uim_log(const uint8_t *sdu, uint32_t size)
{
  printf("COACHZ baseband: the UIM answer: error %u, %u TLVs\n", bb_uim.error, bb_uim.tlv_count);
  /* The answer's own bytes. The TLV layout of the slot status is not in this tree, so the bytes are what
     settles it: an answer that names a card and one that does not look different in a way that can be read off
     the log. */
  printf("COACHZ baseband: the answer is %u bytes:", size);
  for (uint32_t i = 0; i < size && i < 96; ++i) printf(" %02x", sdu[i]);
  printf("\n");
  printf("COACHZ baseband: the answer carries %u TLVs:", bb_uim.tlv_count);
  for (uint32_t i = 0; i < bb_uim.tlv_count; ++i)
    printf(" type 0x%02x of %u bytes", bb_uim.tlv_types[i], bb_uim.tlv_lens[i]);
  printf("\n");
}

// One read-only request to one of the radio's services.
//
// The request asks for the state and nothing else: it does not set the radio's mode, it does not register, and it
// writes nothing to the card, so it is safe to send once when the modem has published the service.
//
// The request is asked once. A queue that was full is asked again on the next poll, and a request that did go
// out is not asked again: the radio's state does not change while the modem runs.
static void bb_radio_ask(struct cbn_probe *probe, int *slot, uint32_t service, uint32_t message, uint32_t txn)
{
  const struct cbq_server *server = cbq_server_find(&bb_qmux, service);
  if (!server) return;
  if (*slot < 0) *slot = cbq_client_alloc(&bb_qmux, CBQ_INTERNAL, service);
  if (*slot < 0) return;
  if (probe->sent || probe->answered) return;
  uint8_t sdu[32];
  const uint32_t n = cbn_build_request(sdu, sizeof sdu, txn, message);
  if (!n) return;
  const uint32_t queued = bb_tx_count;
  bb_queue_tx(CBQ_PORT_BASE + (uint32_t)*slot, server->node, server->port, sdu, n);
  if (bb_tx_count != queued) probe->sent = 1;
}

/* The first pair goes out at bring-up, when the modem's NAS is still initialising and answers
   result 1, error 0x4a. Ask again at 10 s intervals, four times, so the log carries what the radio
   says once it is ready: signal strength and the serving system's registration state. */
static void bb_radio_again(void)
{
  if (bb_radio_tries >= 4) return;
  const uint32_t now = bb_now_ms();
  if (bb_radio_next && now < (uint32_t)bb_radio_next) return;
  const struct cbq_server *nas = cbq_server_find(&bb_qmux, CBN_SVC_NAS);
  if (!nas) return;
  ++bb_radio_tries;
  bb_radio_next = (int)(now + 10000u);
  uint8_t sdu[32];
  uint32_t n = cbn_build_request(sdu, sizeof sdu, 0x60u + (uint32_t)bb_radio_tries, CBN_NAS_GET_SIGNAL_INFO);
  if (n && bb_signal_slot >= 0)
    bb_queue_tx(CBQ_PORT_BASE + (uint32_t)bb_signal_slot, nas->node, nas->port, sdu, n);
  n = cbn_build_request(sdu, sizeof sdu, 0x70u + (uint32_t)bb_radio_tries, CBN_NAS_GET_SERVING_SYSTEM);
  if (n && bb_serving_slot >= 0)
    bb_queue_tx(CBQ_PORT_BASE + (uint32_t)bb_serving_slot, nas->node, nas->port, sdu, n);
  printf("COACHZ radio: signal and serving system asked again %d, t %u\n", bb_radio_tries, now);
}

/* ---- NAS indications to the host ----------------------------------------------------------------
 *
 * The host subscribes (Register Indications 0x03, cid 1/4/5/6) and the modem answers the subscription,
 * but the modem sends no indication when the registration finishes: the host's view is only what it polls,
 * and what it polls while the radio was still coming up. So the state the modem just reported is also
 * delivered as the indication the host subscribed to: Serving System (0x24) when the registration or the
 * network changes, Signal Info (0x51) when the signal changes. The bytes are the modem's own answer's
 * TLVs (the result TLV is dropped, an indication has none), so the host parses exactly what the modem said.
 */
static void bb_nas_indicate(uint32_t msg, const uint8_t *sdu, uint32_t size)
{
  static uint8_t have_reg, last_reg, have_net;
  static uint16_t last_mcc, last_mnc;
  static uint8_t have_sig;
  static int16_t last_rsrp;
  if (msg == 0x24u) {
    if (have_reg && have_net && last_reg == bb_radio.reg_state && last_mcc == bb_radio.mcc && last_mnc == bb_radio.mnc) return;
    have_reg = 1u; last_reg = bb_radio.reg_state;
    if (bb_radio.have_plmn) { have_net = 1u; last_mcc = bb_radio.mcc; last_mnc = bb_radio.mnc; }
  } else {
    if (have_sig && last_rsrp == bb_radio.rsrp) return;
    have_sig = 1u; last_rsrp = bb_radio.rsrp;
  }
  uint8_t ind[512];
  uint32_t n = 0;
  ind[n++] = 0x04u; ind[n++] = 0x00u; ind[n++] = 0x00u;
  ind[n++] = (uint8_t)(msg & 0xffu); ind[n++] = (uint8_t)(msg >> 8);
  const uint32_t len_at = n; n += 2u;
  const uint32_t end = (size >= 7u) ? 7u + (uint32_t)(sdu[5] | sdu[6] << 8) : 0u;
  for (uint32_t at = 7u; at + 3u <= end;) {
    const uint32_t type = sdu[at];
    const uint32_t l = (uint32_t)(sdu[at + 1] | sdu[at + 2] << 8);
    if (at + 3u + l > end) break;
    if (type != CBN_TLV_RESULT) { memcpy(ind + n, sdu + at, 3u + l); n += 3u + l; }
    at += 3u + l;
  }
  ind[len_at] = (uint8_t)((n - 7u) & 0xffu); ind[len_at + 1] = (uint8_t)((n - 7u) >> 8);
  uint32_t sent = 0, hosts = 0, dropped = 0;
  for (uint32_t i = 0; i < CBQ_MAX_CLIENTS; i++) {
    const struct cbq_client *c = &bb_qmux.client[i];
    if (!c->used || c->service != CBN_SVC_NAS || c->channel == CBQ_INTERNAL) continue;
    ++hosts;
    const struct cbq_result r = cbq_from_modem(&bb_qmux, CBQ_PORT_BASE + i, 0, ind, n, bb_out, sizeof bb_out);
    if (r.kind == CBQ_TO_HOST && r.len) { bb_push(r.channel ? CBH_QMUX2 : CBH_QMUX1, bb_out, r.len); ++sent; }
    else ++dropped;
  }
  printf("COACHZ nas-indicate: message 0x%x: %u host NAS clients, %u sent, %u refused\n", msg, hosts, sent, dropped);
}

/* ---- registration: what the UMI port does and this bridge did not ----------------------------------
 *
 * The modem does not register on its own. UMI's port (drivers/modem/hakios.mpss.c, sim_probe) sends three NAS
 * requests after the radio is online -- Get System Selection Preference (0x34), Initiate Network Register
 * (0x22, TLV 1 = automatic), Get System Info (0x4d) -- and only then does the serving system turn registered.
 * This bridge only asked (0x24/0x4f): it read "not registered" back because nobody had asked the modem to
 * register. The host cannot fill the gap either: CommCenter's QMUX registration path went through requests
 * that this bridge drops, and the Apple private services it would use are not on this modem.
 *
 * The three asks go on one client of the bridge's own, and the serving system and the signal are asked again
 * on it every two seconds until registered, so the state the log shows is the state the modem is in. */
static void bb_radio_log(void);
static int bb_reg_slot = -1;
static struct cbn_probe bb_reg;
static uint8_t bb_reg_step;
static uint32_t bb_reg_txn = 0x5100u;
static uint32_t bb_reg_next_ms;
static int bb_reg_tries;

static void bb_register_step(void)
{
  const struct cbq_server *nas = cbq_server_find(&bb_qmux, CBN_SVC_NAS);
  if (!nas) return;
  if (bb_reg_slot < 0) bb_reg_slot = cbq_client_alloc(&bb_qmux, CBQ_INTERNAL, CBN_SVC_NAS);
  if (bb_reg_slot < 0) return;
  const uint32_t port = CBQ_PORT_BASE + (uint32_t)bb_reg_slot;
  const uint32_t now = bb_now_ms();
  uint8_t sdu[32];
  if (bb_reg_step == 0u) {
    uint32_t n = cbn_build_request(sdu, sizeof sdu, ++bb_reg_txn, CBN_NAS_GET_SYSTEM_SELECTION);
    if (n) bb_queue_tx(port, nas->node, nas->port, sdu, n);
    n = 0;
    sdu[n++] = 0x00u;
    sdu[n++] = (uint8_t)bb_reg_txn; sdu[n++] = (uint8_t)(bb_reg_txn >> 8);
    sdu[n++] = (uint8_t)(CBN_NAS_INITIATE_NETWORK_REGISTER & 0xffu);
    sdu[n++] = (uint8_t)(CBN_NAS_INITIATE_NETWORK_REGISTER >> 8);
    sdu[n++] = 0x04u; sdu[n++] = 0x00u;                                   /* one TLV, four bytes */
    sdu[n++] = CBN_TLV_REGISTER_ACTION; sdu[n++] = 0x01u; sdu[n++] = 0x00u; sdu[n++] = 0x01u;  /* automatic */
    ++bb_reg_txn;
    bb_queue_tx(port, nas->node, nas->port, sdu, n);
    n = cbn_build_request(sdu, sizeof sdu, ++bb_reg_txn, CBN_NAS_GET_SYSTEM_INFO);
    if (n) bb_queue_tx(port, nas->node, nas->port, sdu, n);
    bb_reg_step = 1u;
    bb_reg_next_ms = now + 3000u;
    printf("COACHZ register: the selection, the automatic registration and the system info asked\n");
    return;
  }
  if (bb_radio.have_serving && bb_radio.reg_state == CBN_REG_REGISTERED) {
    if (bb_reg_step != 2u) {
      bb_reg_step = 2u;
      bb_radio_log();
      printf("COACHZ register: the modem is registered\n");
    }
    return;
  }
  if (bb_reg_tries >= 30 || now < bb_reg_next_ms) return;
  bb_reg_next_ms = now + 2000u;
  ++bb_reg_tries;
  /* The SIM only becomes READY (UIM application state 7) well after the first register request, so the
     automatic registration asked for at bring-up is asked again every five polls (ten seconds): the first
     one is answered 'no effect' while the card is still being initialised, and the modem registers only
     once the USIM is ready (UMI's sim_register_again does the same). */
  if (bb_reg_tries % 5 == 0) {
    uint8_t auto_reg[11];
    uint32_t k = 0;
    auto_reg[k++] = 0x00u;
    auto_reg[k++] = (uint8_t)bb_reg_txn; auto_reg[k++] = (uint8_t)(bb_reg_txn >> 8);
    auto_reg[k++] = (uint8_t)(CBN_NAS_INITIATE_NETWORK_REGISTER & 0xffu);
    auto_reg[k++] = (uint8_t)(CBN_NAS_INITIATE_NETWORK_REGISTER >> 8);
    auto_reg[k++] = 0x04u; auto_reg[k++] = 0x00u;
    auto_reg[k++] = CBN_TLV_REGISTER_ACTION; auto_reg[k++] = 0x01u; auto_reg[k++] = 0x00u; auto_reg[k++] = 0x01u;
    ++bb_reg_txn;
    bb_queue_tx(port, nas->node, nas->port, auto_reg, k);
    printf("COACHZ register: the automatic registration asked again (%u)\n", bb_reg_tries);
  }
  uint32_t n = cbn_build_request(sdu, sizeof sdu, ++bb_reg_txn, CBN_NAS_GET_SERVING_SYSTEM);
  if (n) bb_queue_tx(port, nas->node, nas->port, sdu, n);
  n = cbn_build_request(sdu, sizeof sdu, ++bb_reg_txn, CBN_NAS_GET_SIGNAL_INFO);
  if (n) bb_queue_tx(port, nas->node, nas->port, sdu, n);
}

// The three requests, in the order they are worth asking in.
static void bb_radio_step(void)
{
  /* Whether the radio is on. A radio that is offline has no signal to report, so the other two are asked
     anyway: they answer with what they know, and an answer of "not registered" is worth having too. */
  bb_radio_ask(&bb_dms, &bb_dms_slot, CBN_SVC_DMS, CBN_DMS_GET_OPERATING_MODE, 0x31u);
  bb_radio_ask(&bb_serving, &bb_serving_slot, CBN_SVC_NAS, CBN_NAS_GET_SERVING_SYSTEM, 0x32u);
  bb_radio_ask(&bb_signal, &bb_signal_slot, CBN_SVC_NAS, CBN_NAS_GET_SIGNAL_INFO, 0x33u);
}

// The radio's state, on the record. The numbers are printed as the modem gave them, next to what they mean.
static const char *bb_mode_name(uint8_t mode)
{
  switch (mode) {
    case CBN_MODE_OFFLINE: return "offline";
    case CBN_MODE_ONLINE: return "online";
    case CBN_MODE_LOW_POWER: return "low power";
    case CBN_MODE_FACTORY_TEST: return "factory test";
    case CBN_MODE_SHUTTING_DOWN: return "shutting down";
    case CBN_MODE_PERSIST: return "persist";
    case CBN_MODE_LOW_POWER_ONLY: return "low power only";
    default: return "a mode this tree does not name";
  }
}

static const char *bb_reg_name(uint8_t state)
{
  switch (state) {
    case CBN_REG_NOT_REGISTERED: return "not registered";
    case CBN_REG_REGISTERED: return "registered";
    case CBN_REG_NOT_REGISTERED_SEARCHING: return "not registered, searching";
    case CBN_REG_REGISTRATION_DENIED: return "registration denied";
    case CBN_REG_UNKNOWN: return "unknown";
    default: return "a state this tree does not name";
  }
}

static void bb_radio_log(void)
{
  if (bb_radio.have_operating_mode)
    printf("COACHZ baseband: the radio is %s\n", bb_mode_name(bb_radio.operating_mode));
  if (bb_radio.have_serving)
    printf("COACHZ baseband: the radio is %s, the packet service is %s\n",
           bb_reg_name(bb_radio.reg_state), bb_radio.ps_attached ? "attached" : "not attached");
  if (bb_radio.have_plmn)
    printf("COACHZ baseband: the radio is on the network %u-%u\n", bb_radio.mcc, bb_radio.mnc);
  if (bb_radio.have_signal)
    printf("COACHZ baseband: the signal is %d dBm, the quality %d, the power %d dBm, the SNR %d dB\n",
           bb_radio.rssi, bb_radio.rsrq, bb_radio.rsrp, bb_radio.snr);
}

// eSIM control runs on the same QRTR owner as the other QMI traffic (bb_lock held).
// Only explicit VINYL commands may switch slots or send an APDU. Idle discovery reads slot status.
static void bb_esim_step(void)
{
  if (!bb_esim_enabled || !bb_esim_in || !bb_esim_resp) return;
  const struct cbq_server *server = cbq_server_find(&bb_qmux, CBE_SVC_UIM);
  if (!server) return;
  if (bb_esim_slot < 0) bb_esim_slot = cbq_client_alloc(&bb_qmux, CBQ_INTERNAL, CBE_SVC_UIM);
  if (bb_esim_slot < 0) return;
  const uint32_t now = bb_now_ms();
  while (!bb_esim.freeze && !bb_esim.route_failed && (bb_esim.slots.known || (bb_esim.step == CBE_S_IDLE && bb_esim.txn)) &&
         bb_uim_count && bb_tx_count < BB_TX) {
    bb_tx &held = bb_uim_held[bb_uim_head];
    const uint32_t index = held.local_port - CBQ_PORT_BASE;
    if (index < CBQ_MAX_CLIENTS && bb_qmux.client[index].used && bb_qmux.client[index].service == CBE_SVC_UIM &&
        bb_qmux.client[index].cid == bb_uim_cid[bb_uim_head]) {
      bb_txq[(bb_tx_head + bb_tx_count++) % BB_TX] = held;
    } else IOFreeData(held.data, held.len);
    bb_uim_head = (bb_uim_head + 1) % BB_TX; bb_uim_count--;
  }
  for (uint32_t i = 0; i < CBE_JOBS; i++) {
    struct cbe_job *j = &bb_esim.job[i];
    if (j->state && cbq_client_find(&bb_qmux, j->channel, CBE_SVC_VINYL, j->cid) < 0) {
      if ((int)i == bb_esim.active) cbe_finish(&bb_esim, 1, CBE_R_INACTIVE, 0, now);
      else j->state = CBE_J_FREE;
    }
  }
  if (bb_esim.session && bb_esim.active < 0 &&
      cbq_client_find(&bb_qmux, bb_esim.session_channel, CBE_SVC_VINYL, bb_esim.session_cid) < 0)
    cbe_session_release(&bb_esim, now, 2);
  cbe_tick(&bb_esim, now);
  if (bb_esim.req_len) {
    const uint32_t queued = bb_tx_count;
    bb_queue_tx(CBQ_PORT_BASE + (uint32_t)bb_esim_slot, server->node, server->port, bb_esim.req, bb_esim.req_len);
    if (bb_tx_count != queued) bb_esim.req_len = 0;
  }
  for (uint32_t i = 0; i < 4 && bb_esim.done_pending; i++) {
    const struct cbe_job *done = &bb_esim.done;
    if (cbq_client_find(&bb_qmux, done->channel, CBE_SVC_VINYL, done->cid) < 0) {
      if (bb_esim.step == CBE_S_IDLE) bb_esim.done_pending = 0;
      break;
    }
    const uint32_t channel = done->channel ? CBH_QMUX2 : CBH_QMUX1;
    if (bb_ch[channel].count == BB_QUEUE) break;
    const uint32_t chunk = bb_esim.done_chunk, pending = bb_esim.done_pending, queued = bb_ch[channel].count;
    uint8_t qmux = 0;
    const uint32_t n = cbe_output(&bb_esim, &qmux, bb_out, sizeof bb_out);
    if (!n) break;
    bb_push(channel, bb_out, n);
    if (bb_ch[channel].count == queued) { bb_esim.done_chunk = chunk; bb_esim.done_pending = (uint8_t)pending; break; }
  }
  struct cbe_event event;
  while (cbe_take_event(&bb_esim, &event))
    printf("COACHZ baseband: esim event %u a%u b%u c%u d%u e%u\n", event.kind, event.a, event.b, event.c, event.d, event.e);
}

// The activation indication (coachz_bb_data.h) to the BSP client that registered, with slot one's ICCID and IMSI once the card gave them.
static uint8_t bb_iccid[CBD_ICCID_BYTES], bb_imsi[CBD_IMSI_BYTES], bb_iccid_known, bb_imsi_known, bb_act_known, bb_act_channel, bb_act_cid;
static void bb_activation_indicate(void)
{
  if (!bb_act_known) return;
  uint8_t tlvs[CBD_ACTIVATION_SIM_TLVS];
  const uint32_t t = cbd_activation_state(tlvs, sizeof tlvs, bb_iccid_known ? bb_iccid : nullptr, bb_imsi_known ? bb_imsi : nullptr);
  const uint32_t n = cbq_private_indication(CBD_BSP_ACTIVATION_REGISTER, CBD_SVC_BSP, bb_act_cid, tlvs, t, bb_out, sizeof bb_out);
  if (n) bb_push(bb_act_channel, bb_out, n);
}

// CommCenter's own EF_ICCID and EF_IMSI reads: the request's client, transaction and file, so the answer can be recognized. Never logged.
struct bb_sim_read { uint8_t used, channel, cid; uint16_t txn, file; };
static bb_sim_read bb_sim_reads[8];
static uint32_t bb_sim_next;
static void bb_sim_request(const struct cbq_result &r, const uint8_t *sdu)
{
  if (r.service != CBD_SVC_UIM) return;
  const uint32_t file = cbd_uim_read_file(sdu, r.len);
  if (file != CBD_EF_ICCID && file != CBD_EF_IMSI) return;
  printf("COACHZ sim: ReadTransparent EF 0x%04x cid %u txn %u (iccid_known %u imsi_known %u act_known %u)\n",
         file, r.cid, (uint32_t)cbd_rd16(sdu + 1), bb_iccid_known, bb_imsi_known, bb_act_known);
  bb_sim_reads[bb_sim_next++ % 8] = { 1, r.channel, r.cid, cbd_rd16(sdu + 1), (uint16_t)file };
}
static void bb_sim_answer(const struct cbq_result &r, const uint8_t *sdu, uint32_t n)
{
  if (r.service != CBD_SVC_UIM || n < 7u || sdu[0] != 2u || cbd_rd16(sdu + 3) != CBD_UIM_READ_TRANSPARENT) return;
  printf("COACHZ sim: ReadTransparent answer cid %u txn %u len %u\n", r.cid, (uint32_t)cbd_rd16(sdu + 1), n);
  for (auto &q : bb_sim_reads) {
    if (!q.used || q.channel != r.channel || q.cid != r.cid || q.txn != cbd_rd16(sdu + 1)) continue;
    q.used = 0;
    uint8_t ef[16], value[CBD_ICCID_BYTES];
    const uint32_t len = cbd_uim_read_content(sdu, n, ef, sizeof ef);
    const bool iccid = q.file == CBD_EF_ICCID;
    if (iccid ? !cbd_iccid_from_ef(ef, len, value) : !cbd_imsi_from_ef(ef, len, value)) return;
    uint8_t *have = iccid ? bb_iccid : bb_imsi, *known = iccid ? &bb_iccid_known : &bb_imsi_known;
    const uint32_t size = iccid ? CBD_ICCID_BYTES : CBD_IMSI_BYTES;
    if (*known && !memcmp(value, have, size)) return;
    memcpy(have, value, size); *known = 1;
    printf("COACHZ baseband: slot one %s read from the card, activation indication %s\n", iccid ? "ICCID" : "IMSI",
           bb_act_known ? "sent again with it" : "waits for registration");
    bb_activation_indicate();
    return;
  }
}

// A private request the bridge answers itself: 1 when it did.
static int bb_private_answer(uint32_t channel, const struct cbq_result &r, const uint8_t *sdu)
{
  const uint32_t message = (uint32_t)(sdu[3] | sdu[4] << 8);
  if (r.service == CBE_SVC_VINYL) {
    const uint32_t n = bb_esim_in && bb_esim_resp ?
      cbe_host(&bb_esim, r.channel, r.cid, sdu, r.len, bb_out, sizeof bb_out, bb_now_ms()) :
      cbq_private_answer(sdu, r.service, r.cid, CBE_ERR_NO_MEMORY, nullptr, 0, bb_out, sizeof bb_out);
    if (!n) return 0;
    if (n != CBE_DEFERRED) bb_push(channel, bb_out, n);
    return 1;
  }
  // Carrier bundle files (coachz_bb_data.h): accepted and dropped, each segment acknowledged with the next segment number.
  if (r.service == CBD_SVC_BSP && message == CBD_BSP_SEND_FILE) {
    struct cbd_file_segment f;
    if (!cbd_bsp_file_segment(sdu, r.len, &f)) return 0;
    uint8_t tlvs[8];
    const uint32_t t = cbd_bsp_file_answer(&f, tlvs, sizeof tlvs);
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, tlvs, t, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    if (f.segment + 1u == f.count) {
      // The file is complete: report it received as an Apple modem does (FileTransferStatus), or CommCenter times out after 22 s.
      uint8_t done[12];
      const uint32_t d = cbd_bsp_file_done(&f, done, sizeof done);
      const uint32_t m = cbq_private_indication(CBD_BSP_FILE_TRANSFER_STATUS, r.service, r.cid, done, d, bb_out, sizeof bb_out);
      if (m) bb_push(channel, bb_out, m);
      printf("COACHZ baseband: carrier file type %u taken (%u segments, last %u bytes), dropped, transfer status sent\n", f.type, f.count, f.bytes);
    }
    return 1;
  }
  if (r.service == CBD_SVC_BSP && message == CBD_BSP_CHIP_INFO) {
    uint8_t tlvs[12];
    const uint32_t t = cbd_bsp_chip_info(CBS_CHIP_ID, CBS_SERIAL, tlvs, sizeof tlvs);
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, tlvs, t, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    return 1;
  }
  if (r.service == CBD_SVC_BSP && message == CBD_BSP_SET_CONFIG) {
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, nullptr, 0, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    printf("COACHZ baseband: baseband config (SetConfig) taken\n");
    return 1;
  }
  if (r.service == CBD_SVC_BSP && message == CBD_BSP_ACTIVATION_REGISTER) {
    const uint32_t n = cbq_private_answer(sdu, r.service, r.cid, 0, 0, 0, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    bb_act_known = 1; bb_act_channel = (uint8_t)channel; bb_act_cid = r.cid;
    bb_activation_indicate();
    if (bb_activations++ < 4)
      printf("COACHZ baseband: activation register answered, state activated, slot one ICCID %s, IMSI %s\n", bb_iccid_known ? "included" : "not read yet",
             bb_imsi_known ? "included" : "not read yet");
    return 1;
  }
  return 0;
}

// One write from ABM on a channel, after its bytes were copied in.
static void bb_host_write(uint32_t channel, const uint8_t *data, uint32_t len)
{
  bb_channel *c = &bb_ch[channel];
  c->frames_in++;
  c->bytes_in += len;
  if (channel == CBH_SAHARA) {
    const enum cbs_state before = bb_sahara.state;
    for (uint32_t at = 0; at < len && bb_sahara.state != CBS_FAILED;) {
      struct cbs_out o;
      const enum cbs_state step = bb_sahara.state;
      const uint32_t used = cbs_receive(&bb_sahara, data + at, len - at, &o);
      /* Which sahara command the upper layer sent and what the state machine answered: the boot end stalls
         between HELLO, DONE and COMMAND_READY, and only the wire order shows which step is missing. */
      const uint32_t cmd = len - at >= 4u ? (uint32_t)data[at] | (uint32_t)data[at + 1] << 8 | (uint32_t)data[at + 2] << 16 | (uint32_t)data[at + 3] << 24 : ~0u;
      printf("COACHZ sahara: rx cmd 0x%x len %u, state %u -> %u, %u out, seg %u/%u have %u expect %u remaining %u",
             cmd, used, (unsigned)step, (unsigned)bb_sahara.state, o.count,
             bb_sahara.segment, bb_sahara.segments, bb_sahara.have, bb_sahara.expect, bb_sahara.read_remaining);
      /* A HELLO response is where the host says which mode it wants next; the two words name it. */
      if (cmd == 0x2u && used >= 24u) {
        const uint32_t *w = (const uint32_t *)(const void *)(data + at);
        printf(", hello status %u mode %u w8 %u w12 %u", w[4], w[5], w[2], w[3]);
      }
      printf("\n");
      for (uint32_t i = 0; i < o.count; i++) {
        const uint8_t *p = o.data[i];
        const uint32_t tcmd = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        const uint32_t tlen = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        const uint32_t timg = (uint32_t)p[8] | (uint32_t)p[9] << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
        const uint32_t toff = (uint32_t)p[12] | (uint32_t)p[13] << 8 | (uint32_t)p[14] << 16 | (uint32_t)p[15] << 24;
        const uint32_t tlen2 = (uint32_t)p[16] | (uint32_t)p[17] << 8 | (uint32_t)p[18] << 16 | (uint32_t)p[19] << 24;
        printf("COACHZ sahara: tx cmd 0x%x len %u img 0x%x off 0x%x len 0x%x\n", tcmd, tlen, timg, toff, tlen2);
        bb_push(CBH_SAHARA, o.data[i], o.len[i]);
      }
      if (!used) break;
      at += used;
    }
    if (bb_sahara.state != before && bb_sahara.state == CBS_COMMAND)
      printf("COACHZ baseband: sahara command mode\n");
    if (bb_sahara.state != before && (bb_sahara.state == CBS_FINISHED || bb_sahara.state == CBS_FAILED || bb_sahara.state == CBS_WAIT_DONE))
      printf("COACHZ baseband: sahara %s, image %u of %u (id 0x%x), host mode %u status %u, last command %u, %u requests, %llu bytes\n",
             bb_sahara.state == CBS_FINISHED ? "finished" : bb_sahara.state == CBS_FAILED ? "failed" : "image read",
             bb_sahara.image_index + 1, bb_sahara.image_count, bb_sahara.images[bb_sahara.image_index], bb_sahara.host_mode,
             bb_sahara.host_status, bb_sahara.last_command, bb_sahara.requests, bb_sahara.bytes);
    return;
  }
  if (channel != CBH_QMUX1 && channel != CBH_QMUX2) return;   // counted above, nothing behind it yet
  const uint32_t qmux = channel == CBH_QMUX1 ? 0 : 1;
  bb_trace_frame("host", qmux + 1u, data, len);
  const struct cbq_result r = cbq_from_host(&bb_qmux, qmux, data, len, bb_out, sizeof bb_out);
  switch (r.kind) {
  case CBQ_TO_HOST:
    bb_push(channel, bb_out, r.len);
    // The first WDS client ABM takes: set the data port up as ModemManager does before its first bind.
    if (r.service == CBD_SVC_WDS && r.local_port && cbq_rd16(data + CBQ_HDR + 2) == CBQ_CTL_GET_CLIENT_ID) bb_data_start();
    if (bb_qmux.ctl_other != bb_ctl_logged) {
      bb_ctl_logged = bb_qmux.ctl_other;
      printf("COACHZ baseband: CTL message 0x%x accepted without action\n", (uint32_t)(data[CBQ_HDR + 2] | data[CBQ_HDR + 3] << 8));
    }
    break;
  case CBQ_TO_MODEM:
    // Apple's report configuration this modem's firmware does not know (coachz_bb_data.h): accepted here, not forwarded.
    if (cbd_vendor_reporting(r.service, data + CBQ_HDR, r.len)) {
      const uint32_t n = cbq_private_answer(data + CBQ_HDR, r.service, r.cid, 0, nullptr, 0, bb_out, sizeof bb_out);
      if (n) bb_push(channel, bb_out, n);
      break;
    }
    bb_to_modem(r, data + CBQ_HDR);
    break;
  case CBQ_PRIVATE: {
    const uint8_t *sdu = data + CBQ_HDR;
    if (bb_private_answer(channel, r, sdu)) break;
    bb_private_note(r.service, (uint32_t)(sdu[3] | sdu[4] << 8));
    const uint32_t n = cbq_private_unsupported(sdu, r.service, r.cid, bb_out, sizeof bb_out);
    if (n) bb_push(channel, bb_out, n);
    break;
  }
  case CBQ_DROP:
    break;
  }
}

// ------------------------------------------------------------------ asynchronous completions
// IOUserClient::sendAsyncResult64 is a protected static; this never-instantiated subclass only forwards to it.
class CoachzBasebandAsync : public IOUserClient {
public:
  static IOReturn send(OSAsyncReference64 reference, IOReturn result, io_user_reference_t *args, UInt32 count)
  {
    return sendAsyncResult64(reference, result, args, count);
  }
};

static void bb_finish_read(bb_read *read, IOReturn result, const uint8_t *data, uint32_t len)
{
  io_user_reference_t args[1] = { 0 };
  if (data && read->memory) {
    const uint32_t n = len < read->len ? len : read->len;
    args[0] = read->memory->writeBytes(0, data, n);
    if (len > read->len) result = kIOReturnOverrun;
  }
  if (read->memory) {
    read->memory->complete();
    read->memory->release();
    read->memory = nullptr;
  }
  CoachzBasebandAsync::send(read->reference, result, args, 1);
}

static void bb_complete(thread_call_param_t, thread_call_param_t)
{
  for (;;) {
    bb_read read;
    bb_frame frame = { 0, nullptr };
    bool found = false;
    IOLockLock(bb_lock);
    for (uint32_t c = 0; c < CBH_PUBLISHED && !found; c++) {
      bb_channel *ch = &bb_ch[c];
      if (!ch->reads_waiting || !ch->count) continue;
      read = ch->reads[0];
      memmove(&ch->reads[0], &ch->reads[1], (--ch->reads_waiting) * sizeof ch->reads[0]);
      frame = bb_pop(c);
      found = true;
    }
    IOLockUnlock(bb_lock);
    if (!found) return;
    bb_finish_read(&read, kIOReturnSuccess, frame.data, frame.len);
    IOFreeData(frame.data, frame.len);
  }
}

// Every read a client has waiting ends with `result` (close, abort).
static void bb_abort_reads(uint32_t channel, void *owner, IOReturn result)
{
  bb_read ended[BB_READS];
  uint32_t n = 0;
  IOLockLock(bb_lock);
  bb_channel *ch = &bb_ch[channel];
  for (uint32_t i = 0; i < ch->reads_waiting;) {
    if (!owner || ch->reads[i].owner == owner) {
      ended[n++] = ch->reads[i];
      memmove(&ch->reads[i], &ch->reads[i + 1], (--ch->reads_waiting - i) * sizeof ch->reads[0]);
    } else {
      i++;
    }
  }
  IOLockWakeup(bb_lock, ch, false);
  IOLockUnlock(bb_lock);
  for (uint32_t i = 0; i < n; i++) bb_finish_read(&ended[i], result, nullptr, 0);
}

// ------------------------------------------------------------------ user memory
static uint8_t *bb_copy_in(task_t task, mach_vm_address_t address, uint32_t len)
{
  if (!len || len > BB_FRAME_MAX) return nullptr;
  IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(address, len, kIODirectionOut, task);
  uint8_t *copy = static_cast<uint8_t *>(IOMallocData(len));
  bool ok = memory && copy && memory->prepare() == kIOReturnSuccess;
  if (ok) {
    ok = memory->readBytes(0, copy, len) == len;
    memory->complete();
  }
  OSSafeReleaseNULL(memory);
  if (!ok && copy) {
    IOFreeData(copy, len);
    copy = nullptr;
  }
  return copy;
}

static bool bb_async(IOExternalMethodArguments *args) { return args->asyncWakePort != MACH_PORT_NULL && args->asyncReference; }

static void bb_reply(IOExternalMethodArguments *args, IOReturn result, uint32_t count, uint64_t a = 0, uint64_t b = 0)
{
  io_user_reference_t values[2] = { a, b };
  OSAsyncReference64 reference;
  memset(reference, 0, sizeof reference);
  memcpy(reference, args->asyncReference, sizeof(io_user_reference_t) *
         (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
  CoachzBasebandAsync::send(reference, result, values, count);
}

// ------------------------------------------------------------------ AppleBasebandPCIInterface
/* Which service ABM opens, and with which client type: the board-class open and the PCI control open say
 * different things about how far the baseband stack came, and a channel open that never happens is the
 * difference between "ABM never touched it" and "ABM stopped after the board class". */
static void bb_note_open(const char *name, UInt32 type, uint32_t *seen)
{
  const uint32_t n = __atomic_fetch_add(seen, 1u, __ATOMIC_RELAXED);
  if (n < 4) printf("COACHZ baseband: %s user client %u, type %u\n", name, n + 1, (unsigned)type);
}

class AppleBasebandPCIUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandPCIUserClient)
  task_t fTask;
  uint32_t fChannel;
public:
  bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override
  {
    fTask = task;
    return IOUserClient::initWithTask(task, security, type, properties);
  }
  void setChannel(uint32_t channel) { fChannel = channel; }
  IOReturn clientClose() override
  {
    closeChannel();
    terminate();
    return kIOReturnSuccess;
  }
  void closeChannel()
  {
    bb_abort_reads(fChannel, this, kIOReturnAborted);
    IOLockLock(bb_lock);
    if (bb_ch[fChannel].owner == this) bb_ch[fChannel].owner = nullptr;
    IOLockUnlock(bb_lock);
  }
  IOReturn write(IOExternalMethodArguments *args);
  IOReturn read(IOExternalMethodArguments *args);
  IOReturn readRegister(IOExternalMethodArguments *args);
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override;
};
OSDefineMetaClassAndStructors(AppleBasebandPCIUserClient, IOUserClient)

IOReturn AppleBasebandPCIUserClient::write(IOExternalMethodArguments *args)
{
  uint8_t *copy = nullptr;
  uint32_t len = 0;
  if (bb_async(args)) {
    if (args->scalarInputCount < 2) return kIOReturnBadArgument;
    len = (uint32_t)args->scalarInput[1];
    copy = bb_copy_in(fTask, args->scalarInput[0], len);
  } else if (args->structureInputDescriptor) {
    IOMemoryDescriptor *memory = args->structureInputDescriptor;
    len = (uint32_t)memory->getLength();
    copy = len && len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
    bool ok = copy && memory->prepare() == kIOReturnSuccess;
    if (ok) {
      ok = memory->readBytes(0, copy, len) == len;
      memory->complete();
    }
    if (!ok && copy) {
      IOFreeData(copy, len);
      copy = nullptr;
    }
  } else if (args->structureInput && args->structureInputSize) {
    len = args->structureInputSize;
    copy = len <= BB_FRAME_MAX ? static_cast<uint8_t *>(IOMallocData(len)) : nullptr;
    if (copy) memcpy(copy, args->structureInput, len);
  }
  if (!copy) return kIOReturnBadArgument;
  IOLockLock(bb_lock);
  bb_host_write(fChannel, copy, len);
  IOLockUnlock(bb_lock);
  IOFreeData(copy, len);
  if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 1, len);
  return kIOReturnSuccess;
}

IOReturn AppleBasebandPCIUserClient::read(IOExternalMethodArguments *args)
{
  if (bb_async(args)) {
    if (args->scalarInputCount < 2 || !args->scalarInput[1]) return kIOReturnBadArgument;
    bb_read r;
    memset(&r, 0, sizeof r);
    memcpy(r.reference, args->asyncReference, sizeof(io_user_reference_t) *
           (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
    r.len = (uint32_t)args->scalarInput[1];
    r.owner = this;
    r.memory = IOMemoryDescriptor::withAddressRange(args->scalarInput[0], r.len, kIODirectionIn, fTask);
    if (!r.memory || r.memory->prepare() != kIOReturnSuccess) {
      OSSafeReleaseNULL(r.memory);
      return kIOReturnVMError;
    }
    IOLockLock(bb_lock);
    bb_channel *ch = &bb_ch[fChannel];
    static uint32_t refs_reads;
    if (fChannel == CBH_REFS && refs_reads++ < 6)
      printf("COACHZ baseband: refs read waiting, request %u bytes, %u in queue\n", r.len, ch->count);
    const bool queued = ch->reads_waiting < BB_READS;
    if (queued) ch->reads[ch->reads_waiting++] = r;
    if (queued && ch->count && bb_complete_call) thread_call_enter(bb_complete_call);
    IOLockUnlock(bb_lock);
    if (!queued) {
      r.memory->complete();
      r.memory->release();
      return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
  }
  // synchronous: the next frame, waiting up to BB_SYNC_READ_SECONDS
  uint64_t deadline = 0;
  clock_interval_to_deadline(BB_SYNC_READ_SECONDS, kSecondScale, &deadline);
  IOLockLock(bb_lock);
  bb_channel *ch = &bb_ch[fChannel];
  int waited = THREAD_AWAKENED;
  while (!ch->count && ch->owner == this && waited == THREAD_AWAKENED)
    waited = IOLockSleepDeadline(bb_lock, ch, deadline, THREAD_INTERRUPTIBLE);
  bb_frame frame = bb_pop(fChannel);
  IOLockUnlock(bb_lock);
  if (!frame.data) return waited == THREAD_TIMED_OUT ? kIOReturnTimeout : kIOReturnAborted;
  IOReturn result = kIOReturnSuccess;
  if (args->structureOutputDescriptor) {
    IOMemoryDescriptor *memory = args->structureOutputDescriptor;
    const uint32_t room = (uint32_t)memory->getLength(), n = frame.len < room ? frame.len : room;
    if (memory->prepare() == kIOReturnSuccess) {
      memory->writeBytes(0, frame.data, n);
      memory->complete();
      args->structureOutputDescriptorSize = n;
    } else {
      result = kIOReturnVMError;
    }
    if (frame.len > room) result = kIOReturnOverrun;
  } else if (args->structureOutput) {
    const uint32_t n = frame.len < args->structureOutputSize ? frame.len : args->structureOutputSize;
    memcpy(args->structureOutput, frame.data, n);
    if (frame.len > args->structureOutputSize) result = kIOReturnOverrun;
    args->structureOutputSize = n;
  } else {
    result = kIOReturnBadArgument;
  }
  IOFreeData(frame.data, frame.len);
  return result;
}

// IOABPClass::readRegister: one scalar, the register, and a structure output of the size asked for.
IOReturn AppleBasebandPCIUserClient::readRegister(IOExternalMethodArguments *args)
{
  if (args->scalarInputCount < 1) return kIOReturnBadArgument;
  const uint32_t reg = (uint32_t)args->scalarInput[0];   // pbl
  static uint32_t seen;
  if (reg < 32 && !(__atomic_fetch_or(&seen, 1u << reg, __ATOMIC_RELAXED) & (1u << reg)))
    printf("COACHZ baseband: %s register %u read, %u bytes\n", cbh_published[fChannel], reg,
           args->structureOutputDescriptor ? (uint32_t)args->structureOutputDescriptor->getLength() : args->structureOutputSize);
  if (args->structureOutput && args->structureOutputSize) {
    cbs_read_register(reg, static_cast<uint8_t *>(args->structureOutput), args->structureOutputSize);
    return kIOReturnSuccess;
  }
  if (IOMemoryDescriptor *memory = args->structureOutputDescriptor) {
    const uint32_t size = (uint32_t)memory->getLength();
    if (!size || size > BB_FRAME_MAX) return kIOReturnBadArgument;
    uint8_t *data = static_cast<uint8_t *>(IOMallocData(size));
    if (!data) return kIOReturnNoMemory;
    cbs_read_register(reg, data, size);
    IOReturn result = memory->prepare();
    if (result == kIOReturnSuccess) {
      memory->writeBytes(0, data, size);
      memory->complete();
      args->structureOutputDescriptorSize = size;
    }
    IOFreeData(data, size);
    return result;
  }
  return kIOReturnBadArgument;
}

// Selectors and shapes: coachz_bb_channels.h (libIOABP's IOConnectCall* sites).
IOReturn AppleBasebandPCIUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
                                                    OSObject *, void *)
{
  if (!args) return kIOReturnBadArgument;
  switch (selector) {
  case 0: {   // open
    IOLockLock(bb_lock);
    bb_channel *ch = &bb_ch[fChannel];
    /* fuxi 2026-11-28 (D24): ABM frees and recreates its PCI transport (stage 2 -> stage 4) without closing the old
       client; the new open of qmux1 then got kIOReturnExclusiveAccess and CommCenter's QMIControl stalled on every
       GET_CLIENT_ID ("qmux: message stalled ... msgid=0x22"), so UIM was never reached ("there is no SIM"). The newest
       open takes the channel over; the previous owner's waiting reads are aborted outside the lock. */
    void *previous = (ch->owner && ch->owner != this) ? ch->owner : nullptr;
    if (previous) {
      static uint32_t told;
      if (told++ < 16) printf("COACHZ baseband: %s taken over by a new open (the previous client never closed)\n", cbh_published[fChannel]);
      ch->owner = nullptr;
    }
    const bool free_channel = !ch->owner || ch->owner == this;
    if (free_channel) {
      ch->owner = this;
      if (!ch->opens++) printf("COACHZ baseband: %s opened\n", cbh_published[fChannel]);
      if (fChannel == CBH_SAHARA && bb_boot_image_sent) {
        bb_boot_image_sent = false;
        bb_sahara_restart();
      }
    }
    IOLockUnlock(bb_lock);
    if (previous) bb_abort_reads(fChannel, previous, kIOReturnAborted);
    return free_channel ? kIOReturnSuccess : kIOReturnExclusiveAccess;
  }
  case 1:     // close
    closeChannel();
    return kIOReturnSuccess;
  case 2:
    return write(args);
  case 3:
    return read(args);
  case 4:     // sendImage (BHI): the modem is already running; the image is accepted and dropped
    printf("COACHZ baseband: %s sendImage %llu bytes dropped\n", cbh_published[fChannel], args->scalarInputCount > 1 ? args->scalarInput[1] : 0ull);
    IOLockLock(bb_lock);
    if (fChannel == CBH_PBL) bb_boot_image_sent = true;
    IOLockUnlock(bb_lock);
    if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 2, args->scalarInputCount > 1 ? args->scalarInput[1] : 0, 0);
    return kIOReturnSuccess;
  case 5:
    return readRegister(args);
  case 6:     // abort channel: pending reads end
    bb_abort_reads(fChannel, this, kIOReturnAborted);
    if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 0);
    return kIOReturnSuccess;
  case 7:     // start channel
  case 9:     // map shared memory: nothing here reads it
  case 10:    // unmap
  case 12:    // deregister time event
    if (bb_async(args)) bb_reply(args, kIOReturnSuccess, 0);
    return kIOReturnSuccess;
  case 11:    // register time event: none is ever sent
    return kIOReturnSuccess;
  default:    // 8 time sync: no time base behind this channel
    printf("COACHZ baseband: channel %s selector %u unsupported\n", cbh_published[fChannel], selector);
    return kIOReturnUnsupported;
  }
}

class AppleBasebandPCIInterface : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIInterface)
  uint32_t fChannel;
public:
  bool setup(uint32_t channel)
  {
    fChannel = channel;
    return setProperty(CBH_PROTOCOL_KEY, cbh_published[channel]);
  }
  // IOABPClass::start_nl puts ABPInterfaceProtocol straight in the IOServiceMatching dictionary. IOKit does not compare a key it does not know: the
  // family's matchPropertyTable does, and without it every client gets whichever channel comes first.
  bool matchPropertyTable(OSDictionary *table, SInt32 *score) override
  {
    return IOService::matchPropertyTable(table, score) && matchProtocol(table);
  }
  bool matchPropertyTable(OSDictionary *table) override
  {
    return IOService::matchPropertyTable(table) && matchProtocol(table);
  }
  bool matchProtocol(OSDictionary *table) const
  {
    OSString *wanted = OSDynamicCast(OSString, table->getObject(CBH_PROTOCOL_KEY));
    if (!wanted) return true;
    const bool same = wanted->isEqualTo(cbh_published[fChannel]);
    if (!same) {
      /* Every channel is asked for every protocol until one matches; print each requested name once so the
         names ABM looks for but the table does not publish show up (a missing boot channel would show here). */
      static char seen[24][24];
      static uint32_t seen_count;
      const char *name = wanted->getCStringNoCopy();
      bool logged = false;
      for (uint32_t i = 0; i < seen_count; i++)
        if (!strncmp(seen[i], name, sizeof seen[i] - 1)) { logged = true; break; }
      if (!logged && seen_count < sizeof seen / sizeof seen[0]) {
        strlcpy(seen[seen_count++], name, sizeof seen[0]);
        printf("COACHZ baseband: channel %s refused protocol \"%s\"\n", cbh_published[fChannel], name);
      }
    }
    return same;
  }
  IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out) override
  {
    if (!out) return kIOReturnBadArgument;
    *out = nullptr;
    static uint32_t seen;
    if (!(__atomic_fetch_or(&seen, 1u << fChannel, __ATOMIC_RELAXED) & (1u << fChannel)))
      printf("COACHZ baseband: first user client on %s, type %u\n", cbh_published[fChannel], (unsigned)type);
    AppleBasebandPCIUserClient *client = OSTypeAlloc(AppleBasebandPCIUserClient);
    if (!client) return kIOReturnNoMemory;
    client->setChannel(fChannel);
    if (!client->initWithTask(task, security, type, properties) || !client->attach(this)) {
      client->release();
      return kIOReturnError;
    }
    if (!client->start(this)) {
      client->detach(this);
      client->release();
      return kIOReturnError;
    }
    *out = client;
    return kIOReturnSuccess;
  }
  IOReturn newUserClient(task_t task, void *security, UInt32 type, IOUserClient **out) override
  {
    return newUserClient(task, security, type, nullptr, out);
  }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIInterface, IOService)

// ------------------------------------------------------------------ AppleBasebandPCIControl
class AppleBasebandPCIControlUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandPCIControlUserClient)
  OSAsyncReference64 fLogs;
  bool fLogsPending;
public:
  void endLogs()
  {
    if (!fLogsPending) return;
    fLogsPending = false;
    io_user_reference_t none[1] = { 0 };
    CoachzBasebandAsync::send(fLogs, kIOReturnAborted, none, 1);
  }
  IOReturn clientClose() override
  {
    endLogs();
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    switch (selector) {
    case 0:   // open
    case 3:   // logger tune
      return kIOReturnSuccess;
    case 1:   // close
      endLogs();
      return kIOReturnSuccess;
    case 2:   // read logs: there are none; the request waits until close
      if (!bb_async(args) || fLogsPending) return kIOReturnBadArgument;
      memset(fLogs, 0, sizeof fLogs);
      memcpy(fLogs, args->asyncReference, sizeof(io_user_reference_t) *
             (args->asyncReferenceCount < kOSAsyncRef64Count ? args->asyncReferenceCount : kOSAsyncRef64Count));
      fLogsPending = true;
      return kIOReturnSuccess;
    default:
      return kIOReturnUnsupported;
    }
  }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIControlUserClient, IOUserClient)

template <class Client>
static IOReturn bb_open(IOService *service, task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out)
{
  if (!out) return kIOReturnBadArgument;
  *out = nullptr;
  Client *client = OSTypeAlloc(Client);
  if (!client) return kIOReturnNoMemory;
  if (!client->initWithTask(task, security, type, properties) || !client->attach(service)) {
    client->release();
    return kIOReturnError;
  }
  if (!client->start(service)) {
    client->detach(service);
    client->release();
    return kIOReturnError;
  }
  *out = client;
  return kIOReturnSuccess;
}

class AppleBasebandPCIControl : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIControl)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    static uint32_t seen;
    bb_note_open("AppleBasebandPCIControl", type, &seen);
    return bb_open<AppleBasebandPCIControlUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleBasebandPCIControl, IOService)

// ------------------------------------------------------------------ AppleBaseband
class AppleBasebandUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleBasebandUserClient)
public:
  IOReturn clientClose() override
  {
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    uint64_t out[4] = { 0 };
    uint32_t nout = args->scalarOutputCount < 4 ? args->scalarOutputCount : 4;
    IOLockLock(bb_lock);
    const int result = cbc_call(&bb_control, selector, args->scalarInput, args->scalarInputCount, out, &nout);
    /* Every BootController step ABM takes: which selector, what it wrote or read, and the two states it reads back.
       The counters in the periodic line only say how many calls happened, not where the state machine stopped. */
    printf("COACHZ board: sel %u in %u", selector, args->scalarInputCount);
    for (uint32_t i = 0; i < args->scalarInputCount && i < 4; i++) printf(" %llx", (unsigned long long)args->scalarInput[i]);
    printf(" out %u", nout);
    for (uint32_t i = 0; i < nout && i < 4; i++) printf(" %llx", (unsigned long long)out[i]);
    printf(" res %d mgr %llu bb %llu\n", result, (unsigned long long)bb_control.manager_state, (unsigned long long)bb_control.baseband_state);
    // A reset or power-on goes back to before SendImage: nothing greets until the next one.
    if (!result && (selector == CBC_RESET_MODEM || selector == CBC_POWERCYCLE || (selector == CBC_POWER && args->scalarInputCount && args->scalarInput[0]))) {
      bb_boot_image_sent = false;
      bb_flush(CBH_SAHARA);
    }
    IOLockUnlock(bb_lock);
    if (result == CBC_UNSUPPORTED) printf("COACHZ baseband: AppleBaseband selector %u unsupported\n", selector);
    for (uint32_t i = 0; i < nout; i++) args->scalarOutput[i] = out[i];
    args->scalarOutputCount = nout;
    return result;
  }
};
OSDefineMetaClassAndStructors(AppleBasebandUserClient, IOUserClient)

/* The board class libTelephonyBaseband searches for: IOServiceMatching("baseband") resolves the name through
 * OSMetaClass and matches a service whose class, or one of its ancestors, is that class (IOService.cpp:7625,
 * OSMetaClass::checkMetaCastWithName). This empty superclass supplies that second name on the very same
 * AppleBaseband object, so no extra service is published and every property and name stays where it is. */
class baseband : public IOService {
  OSDeclareDefaultStructors(baseband)
};
OSDefineMetaClassAndStructors(baseband, IOService)

class AppleBaseband : public baseband {
  OSDeclareDefaultStructors(AppleBaseband)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    /* The one open that told us nothing until now: the PCI control and every channel print, this did not, so a log
       without it could not tell "ABM never touched the baseband" from "ABM opened it and stopped". */
    static uint32_t seen;
    bb_note_open("AppleBaseband", type, &seen);
    return bb_open<AppleBasebandUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleBaseband, baseband)

#include "coachz_bb_pdp.cpp"

// ------------------------------------------------------------------ the data plane's kernel services
// ABM's data::TransportService calls ApplePDPHelperInterface::getInstance()->enableMetadataTrailer() without a null check, and getInstance
// is null without an AppleIPAppender service to open. ApplePDPHelper picks its variant by which PDP manager is registered; this is the Mav
// one. Nothing is behind them until pdp_ip exists: every AppleIPAppender method (0 addFlow ... 20 registerFilterRuleCallBack, libApplePDPHelper)
// succeeds with zeroed outputs, and the first use of each selector is logged.
class AppleIPAppenderUserClient : public IOUserClient {
  OSDeclareDefaultStructors(AppleIPAppenderUserClient)
public:
  IOReturn clientClose() override
  {
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    // Capture the first call shape for each helper operation, including the traffic gate.
    // Payloads can contain network identities. Record only sizes and counts.
    static uint32_t shapes_seen;
    if (selector < 32 && !(__atomic_fetch_or(&shapes_seen, 1u << selector, __ATOMIC_RELAXED) & (1u << selector)))
      printf("COACHZ pdp: appender selector %u scalar-in %u scalar-out %u struct-in %llu struct-out %llu descriptor-in %llu descriptor-out %llu\n",
        selector, args->scalarInputCount, args->scalarOutputCount,
        static_cast<unsigned long long>(args->structureInputSize), static_cast<unsigned long long>(args->structureOutputSize),
        static_cast<unsigned long long>(args->structureInputDescriptor ? args->structureInputDescriptor->getLength() : 0),
        static_cast<unsigned long long>(args->structureOutputDescriptor ? args->structureOutputDescriptor->getLength() : 0));
    if (selector == 16) {
      if (args->scalarInputCount != 2) return kIOReturnBadArgument;
      return bb_pdp_block(static_cast<uint32_t>(args->scalarInput[0]), static_cast<uint32_t>(args->scalarInput[1]));
    }
    static uint32_t seen;
    if (selector < 32 && !(__atomic_fetch_or(&seen, 1u << selector, __ATOMIC_RELAXED) & (1u << selector)))
      printf("COACHZ baseband: AppleIPAppender selector %u answered with zeros\n", selector);
    for (uint32_t i = 0; i < args->scalarOutputCount; i++) args->scalarOutput[i] = 0;
    if (args->structureOutput && args->structureOutputSize) bzero(args->structureOutput, args->structureOutputSize);
    if (args->structureOutputDescriptor) args->structureOutputDescriptorSize = 0;
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(AppleIPAppenderUserClient, IOUserClient)

class AppleIPAppender : public IOService {
  OSDeclareDefaultStructors(AppleIPAppender)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    static uint32_t seen;
    bb_note_open("AppleIPAppender", type, &seen);
    return bb_open<AppleIPAppenderUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(AppleIPAppender, IOService)

/* fuxi 2026-11-27 (D9): the iPhone ABM asserts in CPMSModule::initialize ("ABM ASSERT ... fCPMSAgent") when
 * +[CPMSAgent sharedCPMSAgent] is nil. CPMS.framework looks the service up by class name (IOServiceMatching "ApplePPM"),
 * reads its CPMSSupported bitfield (bit per PPM client id), opens a user client and registers for interest notifications;
 * without the service the agent is never made. This board has no power/performance manager, so a stand-in answers every
 * call with success and zeros (no budget limits). Each selector's shape is printed once. */
class ApplePPMUserClient : public IOUserClient {
  OSDeclareDefaultStructors(ApplePPMUserClient)
public:
  IOReturn clientClose() override
  {
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override
  {
    if (!args) return kIOReturnBadArgument;
    static uint32_t seen;
    if (selector < 32 && !(__atomic_fetch_or(&seen, 1u << selector, __ATOMIC_RELAXED) & (1u << selector)))
      printf("COACHZ ppm: selector %u scalar-in %u scalar-out %u struct-in %llu struct-out %llu, answered with zeros\n",
        selector, args->scalarInputCount, args->scalarOutputCount,
        static_cast<unsigned long long>(args->structureInputSize), static_cast<unsigned long long>(args->structureOutputSize));
    for (uint32_t i = 0; i < args->scalarOutputCount; i++) args->scalarOutput[i] = 0;
    if (args->structureOutput && args->structureOutputSize) bzero(args->structureOutput, args->structureOutputSize);
    if (args->structureOutputDescriptor) args->structureOutputDescriptorSize = 0;
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(ApplePPMUserClient, IOUserClient)

class ApplePPM : public IOService {
  OSDeclareDefaultStructors(ApplePPM)
public:
  IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
  {
    static uint32_t seen;
    bb_note_open("ApplePPM", type, &seen);
    return bb_open<ApplePPMUserClient>(this, t, s, type, p, out);
  }
  IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override { return newUserClient(t, s, type, nullptr, out); }
};
OSDefineMetaClassAndStructors(ApplePPM, IOService)

// libPCITransport's pci::event::PDPListener matches the base class, ApplePDPHelper the leaf and AppleBasebandPCIPDPManager.
class AppleBasebandPCIPDPManagerBase : public IOService {
  OSDeclareDefaultStructors(AppleBasebandPCIPDPManagerBase)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIPDPManagerBase, IOService)

class AppleBasebandPCIPDPManager : public AppleBasebandPCIPDPManagerBase {
  OSDeclareDefaultStructors(AppleBasebandPCIPDPManager)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIPDPManager, AppleBasebandPCIPDPManagerBase)

class AppleBasebandPCIMAVPDPManager : public AppleBasebandPCIPDPManager {
  OSDeclareDefaultStructors(AppleBasebandPCIMAVPDPManager)
};
OSDefineMetaClassAndStructors(AppleBasebandPCIMAVPDPManager, AppleBasebandPCIPDPManager)

// ------------------------------------------------------------------ publication and the Wi-Fi thread's entry points
/* `node`: a device-tree node whose name and properties the service takes, the way a DT-backed nub would.
 * libTelephonyBaseband finds the board with IOServiceNameMatching("baseband") and reads its "config"
 * (_TelephonyBasebandControllerLoadConfigOnce), so a service published without the node's name and properties
 * is invisible to the baseband manager: ABM never opens it. Same publication the reference implementation uses. */
template <class Service>
static Service *bb_publish(IOService *provider, uint32_t channel = ~0u, const char *node = nullptr)
{
  Service *service = OSTypeAlloc(Service);
  bool ok = service && service->init();
  if (ok && node) {
    IORegistryEntry *dt = IORegistryEntry::fromPath(node, gIODTPlane);
    OSDictionary *props = dt ? dt->dictionaryWithProperties() : nullptr;
    OSCollectionIterator *keys = props ? OSCollectionIterator::withCollection(props) : nullptr;
    while (OSSymbol *key = keys ? OSDynamicCast(OSSymbol, keys->getNextObject()) : nullptr)
      service->setProperty(key, props->getObject(key));
    service->setName(dt ? dt->getName() : "baseband");
    printf("COACHZ baseband: %s took %u properties from IODeviceTree:%s (config %s), name %s\n",
           service->getMetaClass()->getClassName(), props ? props->getCount() : 0u, node,
           props && props->getObject("config") ? "present" : "MISSING", service->getName());
    OSSafeReleaseNULL(keys);
    OSSafeReleaseNULL(props);
    OSSafeReleaseNULL(dt);
  }
  if (ok && channel != ~0u) {
    AppleBasebandPCIInterface *interface = OSDynamicCast(AppleBasebandPCIInterface, service);
    ok = interface && interface->setup(channel);
  }
  ok = ok && service->attach(provider);
  if (ok && !service->start(provider)) {
    service->detach(provider);
    ok = false;
  }
  if (!ok) {
    OSSafeReleaseNULL(service);
    return nullptr;
  }
  service->registerService();
  return service;
}

// The modem's QRTR is up (GLINK started): publish the services ABM looks for. `send` and `del_client` run on the Wi-Fi thread only.
extern "C" void coachz_baseband_configure(int (*send)(uint32_t, uint32_t, uint32_t, const void *, uint32_t), void (*del_client)(uint32_t))
{
  if (!coachz_baseband_enabled() || bb_published) return;
  bb_init();
  if (!bb_lock) return;
  bb_pdp_init();
  bb_send = send;
  bb_del_client = del_client;
  {
    uint32_t once = 0;
    if (PE_parse_boot_argn("sheng-bb-lpm", &once, sizeof once) && once) bb_lpm_once = 1;
    uint32_t esim = 0;
    if (PE_parse_boot_argn("coachz-esim", &esim, sizeof esim) && esim) bb_esim_enabled = 1;
    printf("COACHZ baseband: the eSIM branch is %s (boot-arg coachz-esim%s)\n",
           bb_esim_enabled ? "on" : "off, UIM passes through to the modem", bb_esim_enabled ? "" : " unset");
  }
  IOService *provider = IOService::getResourceService();
  int made = 0;
  made += provider && bb_publish<AppleBaseband>(provider, ~0u, "/baseband");
  made += provider && bb_publish<AppleBasebandPCIControl>(provider);
  for (uint32_t c = 0; c < CBH_PUBLISHED; c++) made += provider && bb_publish<AppleBasebandPCIInterface>(provider, c);
  made += provider && bb_publish<AppleBasebandPCIMAVPDPManager>(provider);
  made += provider && bb_publish<AppleIPAppender>(provider);
  if (ApplePPM *ppm = provider ? OSTypeAlloc(ApplePPM) : nullptr) {   // fuxi 2026-11-27: CPMS stand-in (see ApplePPM)
    if (ppm->init()) {
      ppm->setProperty("CPMSSupported", 0xffffffffULL, 32);
      if (ppm->attach(provider) && ppm->start(provider)) { ppm->registerService(); printf("COACHZ ppm: ApplePPM published, CPMSSupported 0xffffffff\n"); }
      else ppm->detach(provider);
    }
    ppm->release();
  }
  bb_published = 1;
  /* The third state: the kernel side is in the registry, so a client can find it by name and open it. */
  printf("FUXI_STAGE 3/4 ioregistry: AppleBaseband, AppleBasebandPCIControl and %u channel ports are registered\n",
         (unsigned)CBH_PUBLISHED);
  printf("COACHZ baseband: %d of %u services published (AppleBaseband, AppleBasebandPCIControl, %u channels, the PDP manager, AppleIPAppender)\n", made, 4u + CBH_PUBLISHED, CBH_PUBLISHED);
}

extern "C" void coachz_baseband_new_server(uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port)
{
  if (!coachz_baseband_enabled()) return;
  bb_init();
  if (!bb_lock) return;
  IOLockLock(bb_lock);
  cbq_server_new(&bb_qmux, service, version_instance, node, port);
  IOLockUnlock(bb_lock);
}

extern "C" void coachz_baseband_del_server(uint32_t service, uint32_t node, uint32_t port)
{
  if (!bb_lock) return;
  IOLockLock(bb_lock);
  cbq_server_del(&bb_qmux, service, node, port);
  if (service == CBD_SVC_WDS) bzero(bb_pdp_clients, sizeof bb_pdp_clients);
  IOLockUnlock(bb_lock);
}

/* The modem to bridge side: a frame the routing cannot deliver never reaches the upper layer, and without a line
   here it looks exactly like a modem that never answered. The DMS operating-mode answer is the one whose absence
   keeps the radio offline, so it is named whatever the routing returned; drops of any other service are named once
   per (service, message) so a flood of unmatched frames cannot bury the log. */
static uint32_t bb_modem_dropped;
static void bb_mdrop_note(uint32_t kind, uint32_t service, uint32_t msg, uint32_t cid, uint32_t len)
{
  struct { uint32_t key, count; } static seen[32];
  const int always = service == 0x2u && (msg == 0x2du || msg == 0x2eu);
  if (kind != CBQ_TO_HOST) bb_modem_dropped++;
  if (!always && kind != CBQ_DROP) return;
  const uint32_t key = kind << 24 | service << 16 | msg;
  for (uint32_t i = 0; i < 32u; i++) {
    if (seen[i].key != key && seen[i].key) continue;
    seen[i].key = key;
    const uint32_t n = ++seen[i].count;
    if (n == 1u || (n & (n - 1u)) == 0u)
      printf("COACHZ mdrop: kind %u svc 0x%x msg 0x%x cid %u len %u (%u, dropped %u)\n",
             kind, service, msg, cid, len, n, bb_modem_dropped);
    return;
  }
}

// QRTR data for one of the bridge's client ports: 1 when the port is the bridge's.
extern "C" int coachz_baseband_qrtr(uint32_t dst_port, uint32_t src_node, uint32_t, const void *body, uint32_t size)
{
  if (dst_port < CBQ_PORT_BASE || dst_port >= CBQ_PORT_BASE + CBQ_MAX_CLIENTS) return 0;
  if (!bb_lock) return 1;
  IOLockLock(bb_lock);
  const struct cbq_result r = cbq_from_modem(&bb_qmux, dst_port, src_node, static_cast<const uint8_t *>(body), size, bb_out, sizeof bb_out);
  {
    const uint8_t *sdu = static_cast<const uint8_t *>(body);
    const uint32_t slot = dst_port >= CBQ_PORT_BASE ? dst_port - CBQ_PORT_BASE : ~0u;
    const bool used = slot < CBQ_MAX_CLIENTS && bb_qmux.client[slot].used;
    /* A drop leaves the result's service field at zero (cbq_from_modem returns before assigning it), so the
       service and the client id are read from the slot the destination port names. A free slot has nothing to
       name them from, and that is itself the answer. */
    const uint32_t svc = used ? (uint32_t)bb_qmux.client[slot].service : 0xffffu;
    const uint32_t cid = used ? (uint32_t)bb_qmux.client[slot].cid : 0xffffu;
    if (r.kind == CBQ_DROP) {
      const char *why =
          dst_port < CBQ_PORT_BASE ? "port below base"
        : slot >= CBQ_MAX_CLIENTS ? "slot out of range"
        : !used ? "client free"
        : bb_qmux.client[slot].private_service ? "private client"
        : size < CBQ_SDU_HDR ? "sdu short"
        : (size >= 7u && CBQ_SDU_HDR + (uint32_t)(sdu[5] | sdu[6] << 8) > size) ? "length past end"
        : !cbq_server_find(&bb_qmux, svc) ? "no server"
        : "server node mismatch";
      char hex[3u * 8u + 1u];
      const uint32_t take = size < 8u ? size : 8u;
      for (uint32_t i = 0; i < take; i++) {
        static const char digits[] = "0123456789abcdef";
        hex[i * 3u + 0u] = digits[sdu[i] >> 4];
        hex[i * 3u + 1u] = digits[sdu[i] & 15u];
        hex[i * 3u + 2u] = ' ';
      }
      hex[take * 3u] = '\0';
      printf("COACHZ mdrop: drop node %u dst_port %u local %u slot %u used %u private %u svc 0x%x cid %u len %u why %s hex %s\n",
             src_node, dst_port, r.local_port, slot, (unsigned)used,
             used ? (unsigned)bb_qmux.client[slot].private_service : 0u, svc, cid, size, why, hex);
    } else {
      /* The service comes from the slot, not from the result: both hold the same value on the delivered path,
         but only the slot's copy is meaningful when the result is still at its initializer. */
      const uint32_t msg = size >= 7u ? (uint32_t)(sdu[3] | sdu[4] << 8) : 0xffffu;
      bb_mdrop_note((uint32_t)r.kind, svc, msg, cid, size);
      bb_dms2e_note("response", dst_port, sdu, size, 0);
    }
  }
  /* The UIM answers CommCenter asks for itself (slot and card status): their bytes are what decides whether the
     card looks ready, and the trace only carries the length. Diagnostic only. */
  if (r.service == CBP_SVC_UIM && size >= 7u) {
    const uint8_t *sdu = static_cast<const uint8_t *>(body);
    const uint32_t m = (uint32_t)(sdu[3] | sdu[4] << 8);
    const uint32_t type = sdu[0];
    if (m == 0x2eu || m == 0x2fu || m == 0x30u || m == 0x31u || m == 0x20u || m == 0x32u || m == 0x47u || m == 0x48u) {
      printf("COACHZ uim: %s msg 0x%x cid %u txn %u len %u bytes:",
             type == 4u ? "indication" : "answer", m, r.cid,
             (uint32_t)(sdu[1] | sdu[2] << 8), size);
      for (uint32_t i = 0; i < size && i < 200u; i++) printf(" %02x", sdu[i]);
      printf("\n");
      /* TLV walk: raw type/length/value offsets, no semantic assumptions. */
      const uint32_t end = 7u + (uint32_t)(sdu[5] | sdu[6] << 8);
      for (uint32_t at = 7u; at + 3u <= end && at + 3u <= size;) {
        const uint32_t t = sdu[at], l = (uint32_t)(sdu[at + 1] | sdu[at + 2] << 8);
        printf("COACHZ uim:   tlv 0x%02x len %u at %u%s\n", t, l, at,
               (at + 3u + l > end || at + 3u + l > size) ? " MALFORMED" : "");
        if (at + 3u + l > end || at + 3u + l > size) break;
        at += 3u + l;
      }
      if (m == 0x2fu) {
        for (uint32_t at = 7u; at + 3u <= end && at + 3u <= size;) {
          const uint32_t t = sdu[at], l = (uint32_t)(sdu[at + 1] | sdu[at + 2] << 8);
          if (at + 3u + l > end || at + 3u + l > size) break;
          if (t == 0x10u && l >= 11u) {
            /* UMi's layout (umi_modem_qmi.h, umi3_apply_uim): v[8] slot count, v[9] slot 1 card state
               (1 = inserted), v[14] application count, v[16] application 1 state (7 = ready). */
            const uint8_t *v = sdu + at + 3;
            const uint32_t slots = v[8], card = v[9];
            const uint32_t apps = l >= 15u ? v[14] : 0u;
            const uint32_t app_state = (l >= 17u && apps) ? v[16] : 0u;
            if (m == 0x2fu && type == 2u && apps && app_state == 7u && !bb_uim_ready) {
              bb_uim_ready = 1;
              printf("COACHZ uim: app ready (7) seen, polls %d, t %u\n", bb_uim_poll_count, bb_now_ms());
            }
            printf("COACHZ uim:   card-status UMi: slots=%u card_state=%u inserted=%u apps=%u app1_state=%u (len %u)\n",
                   slots, card, (slots != 0u && card == 1u) ? 1u : 0u, apps, app_state, l);
            if ((m == 0x2fu && type == 2u) || (m == 0x32u && type == 4u)) bb_uim_session_try(v, l);
          }
          at += 3u + l;
        }
      }
    }
  }
  /* Diagnostic: the answers the host's own NAS clients get, so the log says whether the registration it can
     see is the modem's registration. */
  if (r.kind == CBQ_TO_HOST && r.channel != CBQ_INTERNAL && r.service == CBN_SVC_NAS) {
    static uint32_t nas_answers;
    if (nas_answers < 16u) {
      const uint8_t *sdu = static_cast<const uint8_t *>(body);
      const uint32_t m = size >= 7u ? (uint32_t)(sdu[3] | sdu[4] << 8) : 0xffffu;
      nas_answers++;
      printf("COACHZ nas-answer: msg 0x%x cid %u len %u (%u)\n", m, (unsigned)r.cid, size, nas_answers);
    }
  }
  /* fuxi 2026-11-28 (D27): the slot status the bridge itself asks for never carried the ICCID in the D26 layout, but
     CommCenter's own card status answers (UIM 0x2f) do: TLV 0x17 { u8 2, u8 10, EF_ICCID bytes, ATR }. Slot one's
     ICCID is taken from the first one that passes the host by, and the activation indication goes out again. */
  if (r.kind == CBQ_TO_HOST && r.channel != CBQ_INTERNAL && r.service == CBP_SVC_UIM && !bb_iccid_known && size >= 7u) {
    const uint8_t *sdu = static_cast<const uint8_t *>(body);
    if ((uint32_t)(sdu[3] | sdu[4] << 8) == 0x2fu) {
      uint32_t l17 = 0;
      const uint8_t *v17 = cbd_find(sdu, size, 0x17, &l17);
      uint8_t value[CBD_ICCID_BYTES];
      if (v17 && l17 >= 2u + CBD_ICCID_BYTES && v17[1] == CBD_ICCID_BYTES &&
          cbd_iccid_from_ef(v17 + 2, CBD_ICCID_BYTES, value)) {
        memcpy(bb_iccid, value, CBD_ICCID_BYTES);
        bb_iccid_known = 1;
        printf("COACHZ baseband: slot one ICCID taken from the card status 0x2f, activation indication %s\n",
               bb_act_known ? "sent again with it" : "waits for registration");
        bb_activation_indicate();
      } else {
        static uint32_t miss;
        if (miss++ < 4u) printf("COACHZ baseband: card status 0x2f without a usable ICCID (tlv 0x17 %u bytes)\n", l17);
      }
    }
  }
  if (r.kind == CBQ_TO_HOST && r.channel == CBQ_INTERNAL) {
    bb_trace("int-rx", 0, r.service, r.cid, static_cast<const uint8_t *>(body), size);
    if (bb_ssctl_slot >= 0 && dst_port == CBQ_PORT_BASE + (uint32_t)bb_ssctl_slot &&
        r.service == CBS_SVC_SSCTL) {
      // The modem's answer to the failure reason request.
      const uint8_t *sdu = static_cast<const uint8_t *>(body);
      /* The TLVs of this answer only: the counter is cleared before the answer is read, so the types the log
         shows are the ones this answer carried. */
      bb_ssctl.tlv_count = 0;
      const int read = cbs_read_answer(&bb_ssctl, sdu, size);
      if (read) bb_ssctl_log(sdu, size);
    } else if (bb_uim_slot >= 0 && dst_port == CBQ_PORT_BASE + (uint32_t)bb_uim_slot &&
               r.service == CBP_SVC_UIM) {
      // The modem's answer to the slot status request.
      const uint8_t *sdu = static_cast<const uint8_t *>(body);
      /* The TLVs of this answer only: the counter is cleared before the answer is read, so the types the log
         shows are the ones this answer carried. */
      bb_uim.tlv_count = 0;
      const int read = cbp_read_answer(&bb_uim, sdu, size);
      if (read) { bb_uim.answered = 1; bb_uim_log(sdu, size); }
      /* fuxi 2026-11-28 (D25): CommCenter took the card for absent ("Not enough SIMs (0)", "there is no SIM") although
         the USIM application is READY: its activation indication carries slot one's SIM only with the ICCID (TLV 0x42),
         and the bridge learnt the ICCID only from CommCenter's own EF reads, which CommCenter never makes without a
         SIM. The slot status answer (0x47) already names it: TLV 0x17 { u8 slots, then per slot u8 length, ICCID }, the
         EF_ICCID bytes, so slot one's ICCID is taken from there and the indication goes out again. */
      if (read && !bb_iccid_known) {
        uint32_t l17 = 0;
        const uint8_t *v17 = cbd_find(sdu, size, 0x17, &l17);
        uint8_t value[CBD_ICCID_BYTES];
        if (v17 && l17 >= 2u + CBD_ICCID_BYTES && v17[0] >= 1u && v17[1] == CBD_ICCID_BYTES &&
            cbd_iccid_from_ef(v17 + 2, CBD_ICCID_BYTES, value)) {
          memcpy(bb_iccid, value, CBD_ICCID_BYTES);
          bb_iccid_known = 1;
          printf("COACHZ baseband: slot one ICCID taken from the slot status, activation indication %s\n",
                 bb_act_known ? "sent again with it" : "waits for registration");
          bb_activation_indicate();
        }
      }
    } else if (bb_dms_slot >= 0 && dst_port == CBQ_PORT_BASE + (uint32_t)bb_dms_slot && bb_lpm_answer(dst_port, static_cast<const uint8_t *>(body), size)) {
      /* The low-power handshake above owns this client while it runs. */
    } else if (bb_dms_slot >= 0 && dst_port == CBQ_PORT_BASE + (uint32_t)bb_dms_slot &&
               r.service == CBN_SVC_DMS) {
      // The modem's answer about the radio's power state.
      const uint8_t *sdu = static_cast<const uint8_t *>(body);
      bb_dms.tlv_count = 0;
      const int read = cbn_read_answer(&bb_dms, sdu, size);
      if (read) {
        bb_dms.answered = 1;
        cbn_take_operating_mode(&bb_radio, sdu, size);
        printf("COACHZ baseband: the DMS answer: error %u, %u TLVs\n", bb_dms.error, bb_dms.tlv_count);
        bb_radio_log();
        if (bb_lpm_once && !bb_lpm_tried) {
          bb_lpm_tried = 1;
          printf("COACHZ baseband: exercising the boot operating-mode handshake (no ABM is driving the baseband)\n");
          bb_lpm_pending = 1;
          bb_lpm_step = 0;
        }
      }
    } else if (bb_reg_slot >= 0 && dst_port == CBQ_PORT_BASE + (uint32_t)bb_reg_slot &&
               r.service == CBN_SVC_NAS) {
      const uint8_t *sdu = static_cast<const uint8_t *>(body);
      bb_reg.tlv_count = 0;
      if (cbn_read_answer(&bb_reg, sdu, size)) {
        const uint32_t m = size >= 7u ? (uint32_t)(sdu[3] | sdu[4] << 8) : 0xffffu;
        if (m == CBN_NAS_GET_SERVING_SYSTEM) {
          cbn_take_serving_system(&bb_radio, sdu, size);
          printf("COACHZ register: the serving system answer: error %u\n", bb_reg.error);
          bb_radio_log();
          bb_nas_indicate(0x24u, sdu, size);
        } else if (m == CBN_NAS_GET_SIGNAL_INFO) {
          const int got = cbn_take_signal(&bb_radio, sdu, size);
          printf("COACHZ register: the signal answer: error %u, %u TLVs, decoded %d\n", bb_reg.error, bb_reg.tlv_count, got);
          for (uint32_t q = 0; q < bb_reg.tlv_count && q < 16u; q++)
            printf("COACHZ register: signal TLV type 0x%02x of %u bytes\n", bb_reg.tlv_types[q], bb_reg.tlv_lens[q]);
          bb_radio_log();
          bb_nas_indicate(0x51u, sdu, size);
        } else {
          printf("COACHZ register: answer message 0x%x: error %u, %u TLVs\n", m, bb_reg.error, bb_reg.tlv_count);
        }
      }
    } else if (bb_serving_slot >= 0 && dst_port == CBQ_PORT_BASE + (uint32_t)bb_serving_slot &&
               r.service == CBN_SVC_NAS) {
      // The modem's answer about the network the radio is on.
      const uint8_t *sdu = static_cast<const uint8_t *>(body);
      bb_serving.tlv_count = 0;
      const int read = cbn_read_answer(&bb_serving, sdu, size);
      if (read) {
        bb_serving.answered = 1;
        cbn_take_serving_system(&bb_radio, sdu, size);
        printf("COACHZ baseband: the serving system answer: error %u, %u TLVs\n", bb_serving.error, bb_serving.tlv_count);
        bb_radio_log();
      }
    } else if (bb_signal_slot >= 0 && dst_port == CBQ_PORT_BASE + (uint32_t)bb_signal_slot &&
               r.service == CBN_SVC_NAS) {
      // The modem's answer about the signal strength.
      const uint8_t *sdu = static_cast<const uint8_t *>(body);
      bb_signal.tlv_count = 0;
      const int read = cbn_read_answer(&bb_signal, sdu, size);
      if (read) {
        bb_signal.answered = 1;
        cbn_take_signal(&bb_radio, sdu, size);
        printf("COACHZ baseband: the signal answer: error %u, %u TLVs\n", bb_signal.error, bb_signal.tlv_count);
        bb_radio_log();
      }
    } else if (bb_esim_slot >= 0 && dst_port == CBQ_PORT_BASE + (uint32_t)bb_esim_slot) {
      cbe_answer(&bb_esim, static_cast<const uint8_t *>(body), size, bb_now_ms());
    } else if (cbd_answer(&bb_data, r.service, static_cast<const uint8_t *>(body), size)) bb_data_step();
  } else if (r.kind == CBQ_TO_HOST) {
    if (r.service == CBD_SVC_WDS) bb_pdp_note(dst_port, r.cid, r.channel, static_cast<const uint8_t *>(body), size);
    // A temporary eUICC card-status indication is not the restored logical SIM.
    const uint8_t *sdu = static_cast<const uint8_t *>(body);
    if (bb_esim_enabled && bb_esim.freeze && r.service == CBE_SVC_UIM && size >= 7 &&
        sdu[0] == CBQ_QMI_INDICATION && cbd_rd16(sdu + 3) == 0x32u) {
      IOLockUnlock(bb_lock);
      return 1;
    }
    const uint32_t fixed = bb_esim_enabled ? cbe_card_status_fix(&bb_esim, r.service,
      static_cast<const uint8_t *>(body), size, bb_fix, sizeof bb_fix) : 0;
    if (fixed) {
      cbq_frame_header(bb_out, fixed, r.service, r.cid);
      memcpy(bb_out + CBQ_HDR, bb_fix, fixed);
      bb_push(r.channel == 0 ? CBH_QMUX1 : CBH_QMUX2, bb_out, CBQ_HDR + fixed);
    } else bb_push(r.channel == 0 ? CBH_QMUX1 : CBH_QMUX2, bb_out, r.len);
    bb_sim_answer(r, sdu, size);   // after the answer itself: bb_activation_indicate reuses bb_out
  }
  IOLockUnlock(bb_lock);
  return 1;
}

// On the Wi-Fi thread: QMI requests for the modem, then DEL_CLIENT for released clients.
extern "C" void coachz_baseband_poll(void)
{
  if (!bb_lock || !bb_send) return;
  /* The pass covers the whole queue, so a destination held at its credit watermark cannot starve the frames
     behind it. Both arrays live in static storage: the poll runs on the Wi-Fi thread, whose stack cannot hold
     two 256-entry copies (the first attempt overflowed the kernel stack). */
  /* fuxi 2026-11-26: heap, 24 KiB of __DATA the iOS 17 fileset does not have. */
  static bb_tx (*batch_heap)[BB_TX];
  if (!batch_heap) batch_heap = (bb_tx (*)[BB_TX])IOMallocZeroData(sizeof *batch_heap);
  if (!batch_heap) return;
  bb_tx (&batch)[BB_TX] = *batch_heap;
  uint32_t n = 0, released[16], r = 0, port;
  const uint32_t now_ms_hold = bb_now_ms();
  IOLockLock(bb_lock);
  bb_ssctl_step();
  bb_uim_step();
  bb_uim_poll_step();
  bb_radio_step();
  bb_radio_again();
  bb_register_step();
  bb_esim_step();
  if (bb_lpm_pending == 1) { bb_lpm_pending = 2; bb_lpm_send(); }
  while (bb_tx_count && n < BB_TX) {
    batch[n++] = bb_txq[bb_tx_head];
    bb_tx_head = (bb_tx_head + 1) % BB_TX;
    bb_tx_count--;
  }
  while (r < 16 && (port = cbq_next_release(&bb_qmux))) released[r++] = port;
  IOLockUnlock(bb_lock);
  /* A send the link refused goes back on the queue: the QRTR layer refuses a DATA packet when the peer is out of
     credit, and the credit comes back with a RESUME_TX. Dropping the request here would lose it with nothing on
     the log to say so. The queue is put back under the lock, in order, so the requests keep their order. */
  uint32_t refused = 0;
  /* QRTR keeps one credit count per destination (node, port), so a destination that is out of credit says nothing
     about the next frame for another destination. The pass therefore blocks only the destination that refused and
     keeps trying the rest; every skipped frame of a blocked destination stays behind the refused one, which keeps
     that destination's FIFO order, and all the kept frames go back in front of the queue in their original order. */
  struct { uint32_t node, port; } blocked[64];
  /* fuxi 2026-11-26: heap, 24 KiB of __DATA the iOS 17 fileset does not have. */
  static bb_tx (*kept_heap)[BB_TX];
  if (!kept_heap) kept_heap = (bb_tx (*)[BB_TX])IOMallocZeroData(sizeof *kept_heap);
  if (!kept_heap) return;
  bb_tx (&kept)[BB_TX] = *kept_heap;
  /* A destination whose last attempt the link refused waits a moment before it is tried again. Without this the
     refused frame comes back to the front of the queue on every poll and the modem's own service is retried
     thousands of times a second, which is what kept the NAS service too busy to register. */
  struct { uint32_t node, port, until; } static hold[64];
  static uint32_t hold_count;
  uint32_t blocked_n = 0, kept_n = 0;
  for (uint32_t i = 0; i < n; i++) {
    bool skip = false;
    for (uint32_t k = 0; k < blocked_n; k++)
      if (blocked[k].node == batch[i].node && blocked[k].port == batch[i].port) skip = true;
    if (skip) {
      kept[kept_n++] = batch[i];
      continue;
    }
    bool held = false;
    for (uint32_t k = 0; k < hold_count; k++)
      if (hold[k].node == batch[i].node && hold[k].port == batch[i].port && (int32_t)(now_ms_hold - hold[k].until) < 0) held = true;
    if (held) {
      kept[kept_n++] = batch[i];
      continue;
    }
    const int sent_rc = bb_send(batch[i].local_port, batch[i].node, batch[i].port, batch[i].data, batch[i].len);
    bb_dms2e_note(sent_rc == 0 ? "sent" : "refused", batch[i].local_port, batch[i].data, batch[i].len, sent_rc);
    if (sent_rc == 0) {
      IOFreeData(batch[i].data, batch[i].len);
      continue;
    }
    bool have = false;
    for (uint32_t k = 0; k < hold_count; k++)
      if (hold[k].node == batch[i].node && hold[k].port == batch[i].port) { hold[k].until = now_ms_hold + 100u; have = true; }
    if (!have && hold_count < 64u) {
      hold[hold_count].node = batch[i].node;
      hold[hold_count].port = batch[i].port;
      hold[hold_count].until = now_ms_hold + 100u;
      hold_count++;
    }
    if (blocked_n < 64u) {
      blocked[blocked_n].node = batch[i].node;
      blocked[blocked_n].port = batch[i].port;
      blocked_n++;
    }
    kept[kept_n++] = batch[i];
    refused++;
  }
  if (kept_n) {
    /* The host enqueues while the batch is being sent: the poll releases the lock before bb_send, so the queue can
       have filled again by now. Take the room that is left, in order, from the front of the kept frames and
       prepend only that much; a frame that no longer fits is freed and counted as a drop, never written over a
       live slot, and bb_tx_count never goes past BB_TX. */
    IOLockLock(bb_lock);
    const uint32_t room = BB_TX - bb_tx_count;
    const uint32_t keep = kept_n < room ? kept_n : room;
    for (uint32_t k = 0; k < keep; k++) {
      const uint32_t pos = (bb_tx_head + BB_TX - keep + k) % BB_TX;
      bb_txq[pos] = kept[k];
    }
    bb_tx_head = (bb_tx_head + BB_TX - keep) % BB_TX;
    bb_tx_count += keep;
    for (uint32_t k = keep; k < kept_n; k++) {
      bb_tx_drop_note("prepend full", kept[k].local_port, kept[k].data, kept[k].len);
      IOFreeData(kept[k].data, kept[k].len);
    }
    IOLockUnlock(bb_lock);
  }
  if (refused) {
    /* The poll runs as fast as the caller calls it, so a request that the link keeps refusing would print on
       every pass and fill the log with millions of lines. The refusals are counted and reported with the rest
       of the bridge's state, once a second, which says the same thing without the flood. */
    bb_tx_refused += refused;
    if (bb_now_ms() - bb_refused_last >= 1000u) {
      bb_refused_last = bb_now_ms();
      printf("COACHZ baseband: %u QMI requests were refused by the link and are tried again\n", bb_tx_refused);
      bb_tx_refused = 0;
    }
  }
  for (uint32_t i = 0; i < r; i++)
    if (bb_del_client) bb_del_client(released[i]);
  bb_pdp_poll();
}

extern "C" void coachz_baseband_report(unsigned second)
{
  if (!bb_lock || !bb_published) return;
  bb_pdp_report(second);
  IOLockLock(bb_lock);
  uint32_t clients = 0, servers = 0;
  for (uint32_t i = 0; i < CBQ_MAX_CLIENTS; i++) clients += bb_qmux.client[i].used;
  for (uint32_t i = 0; i < CBQ_MAX_SERVERS; i++) servers += bb_qmux.server[i].present;
  printf("COACHZ baseband: %u s: %u QMI services, %u clients, dropped %u/%u; sahara state %u, %u executes, %u requests; board calls %u (%u unsupported); data port step %u, %u endpoints named, %u activations\n",
         second, servers, clients, bb_qmux.dropped, bb_tx_dropped, bb_sahara.state, bb_sahara.executes, bb_sahara.requests, bb_control.calls, bb_control.unsupported,
         bb_data.step, bb_endpoint_fixes, bb_activations);
  /* The services the modem announced, by their QMI service id. The ids are what the client looks the service up by,
     so a service the modem offered but the bridge never named shows up here as an id nobody asked for. */
  for (uint32_t i = 0; i < CBQ_MAX_SERVERS; i++) {
    const struct cbq_server *s = &bb_qmux.server[i];
    if (!s->present) continue;
    printf("COACHZ baseband: %u s: the modem offers service 0x%02x, version %u, instance %u, at node %u port %u\n",
           second, s->service, s->version, s->instance, s->node, s->port);
  }
  printf("COACHZ qmi: %u s: %u trace lines, %u held back by the budget\n", second, bb_trace_lines, bb_trace_held);
  /* What the modem says about itself. The failure reason is the modem's own statement of why it stopped,
     and it is the one thing the log cannot work out on its own. */
  if (bb_ssctl.answered)
    printf("COACHZ baseband: %u s: the modem's failure reason: error %u, %u bytes of reason\n", second,
           bb_ssctl.error, bb_ssctl.reason_len);
  else if (bb_ssctl.sent)
    printf("COACHZ baseband: %u s: the modem's failure reason was asked for, no answer yet\n", second);
  /* The state of the card. */
  if (bb_uim.answered)
    printf("COACHZ baseband: %u s: the card's slot status: error %u, %u TLVs\n", second, bb_uim.error, bb_uim.tlv_count);
  else if (bb_uim.sent)
    printf("COACHZ baseband: %u s: the card's slot status was asked for, no answer yet\n", second);
  /* The state of the radio. */
  if (bb_radio.have_operating_mode || bb_radio.have_serving || bb_radio.have_signal) {
    printf("COACHZ baseband: %u s: the radio:", second);
    if (bb_radio.have_operating_mode) printf(" %s", bb_mode_name(bb_radio.operating_mode));
    if (bb_radio.have_serving) printf(" %s", bb_reg_name(bb_radio.reg_state));
    if (bb_radio.have_plmn) printf(" on %u-%u", bb_radio.mcc, bb_radio.mnc);
    if (bb_radio.have_signal) printf(", the signal %d dBm", bb_radio.rssi);
    printf("\n");
  } else if (bb_dms.sent || bb_serving.sent || bb_signal.sent) {
    printf("COACHZ baseband: %u s: the radio was asked for, no answer yet\n", second);
  }
  for (uint32_t c = 0; c < CBH_PUBLISHED; c++) {
    const bb_channel *ch = &bb_ch[c];
    if (ch->opens)
      printf("COACHZ baseband: %s: opens %u, in %u frames %u bytes, out %u frames %u bytes, dropped %u, queued %u, reads waiting %u\n",
             cbh_published[c], ch->opens, ch->frames_in, ch->bytes_in, ch->frames_out, ch->bytes_out, ch->frames_dropped, ch->count,
             ch->reads_waiting);
  }
  IOLockUnlock(bb_lock);
}

/* fuxi 2026-11-26: the iOS 17 fileset has no __DATA room for these, so they live on the heap. */
static bool bb_heap(void)
{
  bool ok = true;
  if (!bb_qmux_heap) bb_qmux_heap = (decltype(bb_qmux_heap))IOMallocZeroData(sizeof *bb_qmux_heap);
  ok = ok && bb_qmux_heap;
  if (!bb_sahara_heap) bb_sahara_heap = (decltype(bb_sahara_heap))IOMallocZeroData(sizeof *bb_sahara_heap);
  ok = ok && bb_sahara_heap;
  if (!bb_ch_heap) bb_ch_heap = (decltype(bb_ch_heap))IOMallocZeroData(sizeof *bb_ch_heap);
  ok = ok && bb_ch_heap;
  if (!bb_txq_heap) bb_txq_heap = (decltype(bb_txq_heap))IOMallocZeroData(sizeof *bb_txq_heap);
  ok = ok && bb_txq_heap;
  if (!bb_out_heap) bb_out_heap = (decltype(bb_out_heap))IOMallocZeroData(sizeof *bb_out_heap);
  ok = ok && bb_out_heap;
  if (!bb_pdp_clients_heap) bb_pdp_clients_heap = (decltype(bb_pdp_clients_heap))IOMallocZeroData(sizeof *bb_pdp_clients_heap);
  ok = ok && bb_pdp_clients_heap;
  if (!bb_fix_heap) bb_fix_heap = (decltype(bb_fix_heap))IOMallocZeroData(sizeof *bb_fix_heap);
  ok = ok && bb_fix_heap;
  if (!bb_esim_heap) bb_esim_heap = (decltype(bb_esim_heap))IOMallocZeroData(sizeof *bb_esim_heap);
  ok = ok && bb_esim_heap;
  if (!bb_uim_held_heap) bb_uim_held_heap = (decltype(bb_uim_held_heap))IOMallocZeroData(sizeof *bb_uim_held_heap);
  ok = ok && bb_uim_held_heap;
  if (!bb_trace_key_heap) bb_trace_key_heap = (decltype(bb_trace_key_heap))IOMallocZeroData(sizeof *bb_trace_key_heap);
  ok = ok && bb_trace_key_heap;
  if (!bb_trace_count_heap) bb_trace_count_heap = (decltype(bb_trace_count_heap))IOMallocZeroData(sizeof *bb_trace_count_heap);
  ok = ok && bb_trace_count_heap;
  return ok;
}
