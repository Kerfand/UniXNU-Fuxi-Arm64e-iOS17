/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The IPC router link to the audio DSP (GLINK channel "IPCRTR", which the DSP opens) and the service locator on it. The DSP's protection
 * domains (audio, charger, sensors) ask the application processor's locator which domains provide a service before they start; the
 * battery manager runs in the charger domain. Started with sheng-adsp=2. Everything runs in the DSP thread's callbacks: each packet is
 * handled at once and any answer is queued with sheng_adsp_send, so nothing here waits. */
#include <stddef.h>
#include <stdint.h>
#include "QcomQrtr.h"
#include <string.h>
#include "ShengAdsp.h"
extern void IOLog(const char *format, ...);

/* [0] stage (1 channel registered, 2 link up, 3 the DSP's hello, negative: registration failed), [1] packets in, [2] data packets in,
 * [3] bad packets, [4] packets out, [5] sends refused, [6] locator requests, [7] locator answers, [8] the DSP's node, [9] services the
 * DSP announced, [10] link ups, [11] link downs, [12] data for ports we do not have, [13] the last locator request's service name
 * length | its first byte << 8 */
volatile int32_t native_qrtr_stats[16];
/* the services the DSP announced, as service:instance in hex */
char native_qrtr_text[96];

enum {kLocalNode = 1, kLocatorPort = 1};

static const char *const kAudio[] = {"avs/audio", NULL};
static const char *const kGps[] = {"gps/gps_service", NULL};
static const QcomPdDomain kDomains[] = {
    {"msm/adsp/audio_pd", 74, kAudio},   {"msm/adsp/root_pd", 74, NULL}, {"msm/adsp/charger_pd", 74, NULL},
    {"msm/adsp/sensor_pd", 74, NULL},    {"msm/cdsp/root_pd", 76, NULL}, {"msm/modem/root_pd", 180, kGps},
};

static QcomQrtr gQrtr;
static ShengAdspChannel *gChannel;

static int send(void *context, const uint8_t *packet, uint32_t length)
{
    (void)context;
    return sheng_adsp_send(gChannel, packet, length) ? -1 : 0;
}

static void hex(char *out, unsigned *at, unsigned cap, uint32_t value)
{
    char digits[8];
    unsigned n = 0;
    do {digits[n++] = "0123456789abcdef"[value & 15]; value >>= 4;} while (value && n < 8);
    while (n && *at + 1 < cap) out[(*at)++] = digits[--n];
}

static void counters(void)
{
    native_qrtr_stats[1] = (int32_t)gQrtr.rx_packets;
    native_qrtr_stats[2] = (int32_t)gQrtr.rx_data;
    native_qrtr_stats[3] = (int32_t)gQrtr.rx_bad;
    native_qrtr_stats[4] = (int32_t)gQrtr.tx_packets;
    native_qrtr_stats[5] = (int32_t)gQrtr.tx_refused;
    native_qrtr_stats[8] = gQrtr.hello_received ? (int32_t)gQrtr.remote_node : -1;
    native_qrtr_stats[9] = (int32_t)gQrtr.remote_count;
    native_qrtr_stats[12] = (int32_t)gQrtr.rx_unrouted;
    if (gQrtr.hello_received && native_qrtr_stats[0] == 2) native_qrtr_stats[0] = 3;
    unsigned at = 0;
    const unsigned cap = sizeof native_qrtr_text;
    for (uint32_t i = 0; i < gQrtr.remote_count && at + 4 < cap; ++i) {
        if (at) native_qrtr_text[at++] = ' ';
        hex(native_qrtr_text, &at, cap, gQrtr.remote[i].service);
        if (at + 1 < cap) native_qrtr_text[at++] = ':';
        hex(native_qrtr_text, &at, cap, gQrtr.remote[i].instance);
    }
    native_qrtr_text[at] = 0;
}

static void locator(void *context, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)context;
    uint8_t answer[480];
    QcomQmiMessage m;
    uint32_t size = 0;
    native_qrtr_stats[6]++;
    if (!qcom_qmi_parse(data, length, &m)) {
        const uint8_t *name = qcom_qmi_tlv(&m, 0x01, &size);
        if (name) {
            native_qrtr_stats[13] = (int32_t)(size | (size ? (uint32_t)name[0] << 8 : 0));
            char sbuf[80];
            uint32_t n = size < sizeof(sbuf) - 1 ? size : (uint32_t)(sizeof(sbuf) - 1);
            for (uint32_t k = 0; k < n; ++k) sbuf[k] = (char)name[k];
            sbuf[n] = 0;
            IOLog("fuxi qrtr loc req #%d msg %#x name \"%s\" (len %u)\n",
                  (int)native_qrtr_stats[6], (unsigned)m.message, sbuf, (unsigned)size);
        }
    }
    const uint32_t n = qcom_servloc_answer(kDomains, sizeof kDomains / sizeof kDomains[0], data, length, answer, sizeof answer);
    if (n && !qcom_qrtr_send_data(q, port, node, from, answer, n)) native_qrtr_stats[7]++;
}

/* fuxi 2026-11-24: the state of a protection domain, asked from the DSP's service registry notifier (service 0x42), the way
 * Linux's PDR client does (drivers/soc/qcom/pdr_interface.c, pdr_register_listener): SERVREG_REGISTER_LISTENER_REQ (0x20) with
 * TLV 0x01 enable = 1 and TLV 0x02 the service path; the response carries TLV 0x10 curr_state, and state changes come as
 * SERVREG_STATE_UPDATED_IND (0x22: TLV 0x01 state, 0x02 path, 0x03 transaction). Log only. */
enum {kPdPort = 2, kServregService = 0x42, kPdPrivateService = 0x4f00};
static uint16_t gPdTxn = 0x40;
static const char *pdStateName(uint32_t s)
{
    return s == 0x1FFFFFFFu ? "UP" : s == 0x0FFFFFFFu ? "DOWN" : s == 0x2FFFFFFFu ? "EARLY_DOWN" : s == 0x7FFFFFFFu ? "UNINIT" : "?";
}
static void pdReceive(void *context, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)context; (void)q; (void)port;
    QcomQmiMessage m;
    if (qcom_qmi_parse(data, length, &m)) {IOLog("fuxi pd: unparsable %u bytes from %u:%u\n", length, node, from); return;}
    uint32_t size = 0, state = 0;
    const uint8_t *result = qcom_qmi_tlv(&m, 0x02, &size);
    uint16_t res = 0, err = 0;
    if (m.type == QCOM_QMI_RESPONSE && result && size >= 4) {res = (uint16_t)(result[0] | result[1] << 8); err = (uint16_t)(result[2] | result[3] << 8);}
    const uint8_t *st = qcom_qmi_tlv(&m, m.type == QCOM_QMI_INDICATION ? 0x01 : 0x10, &size);
    if (st && size >= 4) state = (uint32_t)st[0] | (uint32_t)st[1] << 8 | (uint32_t)st[2] << 16 | (uint32_t)st[3] << 24;
    char path[72] = "";
    const uint8_t *p = m.type == QCOM_QMI_INDICATION ? qcom_qmi_tlv(&m, 0x02, &size) : NULL;
    if (p) {uint32_t n = size < sizeof path - 1 ? size : (uint32_t)sizeof path - 1; for (uint32_t k = 0; k < n; ++k) path[k] = (char)p[k]; path[n] = 0;}
    IOLog("fuxi pd: %s msg %#x txn %u result %u/%u state %#x (%s) %s\n", m.type == QCOM_QMI_INDICATION ? "indication" : "response",
          (unsigned)m.message, (unsigned)m.transaction, res, err, state, st ? pdStateName(state) : "none", path);
}
/* fuxi 2026-11-25: DIAG over QRTR (andersson/diag router/peripheral-qrtr.c). The application processor publishes service 4097;
 * the DIAG instance sits in the field's version byte (cntl = base + 0, data = base + 2), so it is passed as the announced instance.
 * ADSP bases: 64 (lpass) and 192 (sensors). The DSP's diag finds the cntl server and talks to it; replies go back to its node:port. */
typedef void (*ShengQrtrDiag)(uint32_t our_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length);
static ShengQrtrDiag gDiagCntl, gDiagData;
static void diagCntlRx(void *c, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *d, uint32_t n)
{
    (void)c; (void)q;
    if (gDiagCntl) gDiagCntl(port, node, from, d, n);
}
static void diagDataRx(void *c, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *d, uint32_t n)
{
    (void)c; (void)q;
    if (gDiagData) gDiagData(port, node, from, d, n);
}
int sheng_qrtr_diag_publish(ShengQrtrDiag cntl, ShengQrtrDiag data)
{
    gDiagCntl = cntl; gDiagData = data;
    int rc = 0;
    rc |= qcom_qrtr_add_service(&gQrtr, 4097, 64, 3, diagCntlRx, NULL);
    rc |= qcom_qrtr_add_service(&gQrtr, 4097, 66, 4, diagDataRx, NULL);
    rc |= qcom_qrtr_add_service(&gQrtr, 4097, 192, 5, diagCntlRx, NULL);
    rc |= qcom_qrtr_add_service(&gQrtr, 4097, 194, 6, diagDataRx, NULL);
    IOLog("fuxi qrtr: published DIAG 4097 (64/66 lpass, 192/194 sensors) rc=%d locals=%u\n", rc, gQrtr.local_count);
    return rc;
}
int sheng_qrtr_send(uint32_t from_port, uint32_t node, uint32_t port, const void *data, uint32_t length)
{
    return qcom_qrtr_send_data(&gQrtr, from_port, node, port, (const uint8_t *)data, length);
}
/* fuxi 2026-11-25 S11d-5: list the remote's services; for every DIAG (4097) server of the DSP, poke it once from our data port (4)
 * so a peripheral that serves data (instead of connecting to ours) learns our address and starts streaming to it. */
extern void IOSleep(unsigned int milliseconds);
void sheng_qrtr_dump_remote(void)
{
    static int poked;
    IOLog("fuxi qrtr: %u remote services\n", gQrtr.remote_count);
    for (uint32_t i = 0; i < gQrtr.remote_count; ++i) {
        uint32_t s = gQrtr.remote[i].service, in = gQrtr.remote[i].instance, n = gQrtr.remote[i].node, po = gQrtr.remote[i].port;
        IOLog("fuxi qrtr:  svc %u (%#x) inst %#x at %u:%u\n", s, s, in, n, po);
        if (s == 4097 && poked < 2) {
            /* S11d-6: real DIAG requests to the DSP's command port, each raw and framed (7e 01 len16 payload 7e); the replies come
             * back to our port 4 and are printed raw by the data handler. 0x00 version, 0x7c extended build id, 0x0c status,
             * 0x1d time, 0x4b 0x12 0x00 0x00 (subsystem diag: core, version), 0x73 log config (get ranges), 0x7d 0x01 msg ssid ranges. */
            static const uint8_t c0[] = {0x00}, c1[] = {0x7c}, c2[] = {0x0c}, c3[] = {0x1d}, c4[] = {0x4b, 0x12, 0x00, 0x00},
                                 c5[] = {0x73, 0, 0, 0, 1, 0, 0, 0}, c6[] = {0x7d, 0x01};
            static const struct {const uint8_t *d; uint32_t n;} cmds[] = {{c0, 1}, {c1, 1}, {c2, 1}, {c3, 1}, {c4, 4}, {c5, 8}, {c6, 2}};
            for (uint32_t k = 0; k < sizeof cmds / sizeof cmds[0]; ++k) {
                int r1 = qcom_qrtr_send_data(&gQrtr, 4, n, po, cmds[k].d, cmds[k].n);
                uint8_t f[16]; f[0] = 0x7e; f[1] = 1; f[2] = (uint8_t)cmds[k].n; f[3] = 0;
                memcpy(f + 4, cmds[k].d, cmds[k].n); f[4 + cmds[k].n] = 0x7e;
                IOSleep(50);
                int r2 = qcom_qrtr_send_data(&gQrtr, 4, n, po, f, cmds[k].n + 5);
                IOLog("fuxi qrtr:  diag cmd %#x to %u:%u raw rc=%d framed rc=%d\n", cmds[k].d[0], n, po, r1, r2);
                IOSleep(50);
            }
            poked++;
        }
    }
}
void sheng_qrtr_lookup(uint32_t service)
{
    const int rc = qcom_qrtr_lookup(&gQrtr, service, 0);
    IOLog("fuxi qrtr: NEW_LOOKUP service %#x rc=%d (remote services now %u)\n", service, rc, gQrtr.remote_count);
}
void sheng_qrtr_pd_query(const char *path)
{
    const int idx = qcom_qrtr_find_remote(&gQrtr, kServregService);
    if (idx < 0) {IOLog("fuxi pd: no service registry notifier (0x42) announced yet\n"); return;}
    static int added;
    if (!added) added = !qcom_qrtr_add_service(&gQrtr, kPdPrivateService, 1, kPdPort, pdReceive, NULL);
    uint8_t msg[7 + 4 + 3 + 64];
    const uint32_t n = (uint32_t)strlen(path) < 64 ? (uint32_t)strlen(path) : 64;
    uint32_t at = 7;
    msg[at++] = 0x01; msg[at++] = 1; msg[at++] = 0; msg[at++] = 1;                        /* enable = 1 */
    msg[at++] = 0x02; msg[at++] = (uint8_t)n; msg[at++] = 0; memcpy(msg + at, path, n); at += n;   /* service path */
    const uint16_t txn = gPdTxn++, len = (uint16_t)(at - 7);
    msg[0] = QCOM_QMI_REQUEST; msg[1] = (uint8_t)txn; msg[2] = (uint8_t)(txn >> 8);
    msg[3] = 0x20; msg[4] = 0; msg[5] = (uint8_t)len; msg[6] = (uint8_t)(len >> 8);
    const int rc = qcom_qrtr_send_data(&gQrtr, kPdPort, gQrtr.remote[idx].node, gQrtr.remote[idx].port, msg, at);
    IOLog("fuxi pd: asked %s (txn %u, notifier %#x at %u:%u) rc=%d\n", path, txn, gQrtr.remote[idx].instance,
          gQrtr.remote[idx].node, gQrtr.remote[idx].port, rc);
}

static void linkState(void *context, int up)
{
    (void)context;
    if (up) {
        native_qrtr_stats[10]++;
        /* a fresh link: forget the old one's hello and services, keep ours */
        gQrtr.hello_sent = gQrtr.hello_received = 0;
        gQrtr.remote_count = 0;
        gQrtr.remote_node = QCOM_QRTR_NODE_BROADCAST;
        native_qrtr_stats[0] = 2;
        qcom_qrtr_start(&gQrtr);
    } else {
        native_qrtr_stats[11]++;
        native_qrtr_stats[0] = 1;
    }
    counters();
}

static void linkReceive(void *context, const uint8_t *data, uint32_t length)
{
    (void)context;
    qcom_qrtr_receive(&gQrtr, data, length);
    counters();
}

void sheng_qrtr_start(void)
{
    static const ShengAdspClient client = {linkState, linkReceive};
    qcom_qrtr_init(&gQrtr, kLocalNode, send, NULL);
    qcom_qrtr_add_service(&gQrtr, QCOM_SERVLOC_SERVICE, QCOM_SERVLOC_INSTANCE, kLocatorPort, locator, NULL);
    gChannel = sheng_adsp_channel("IPCRTR", 1024, 8, 0, &client, NULL);
    native_qrtr_stats[0] = gChannel ? 1 : -1;
}

QcomQrtr *sheng_qrtr_get(void)
{
    return &gQrtr;
}
