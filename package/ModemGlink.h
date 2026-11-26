/* fuxi 2026-11-26: from ljhhkx529/unixnu-fuxi (QcomGlink.h), the modem GLINK/QRTR renamed modem_glink_/modem_qrtr_. */
/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* GLINK over shared memory: the native GLINK protocol (version handshake, channel open/close, receive intents, data, done) on a pair of
 * SMEM FIFOs. Event driven and allocation free: the owner calls modem_glink_poll whenever the remote rings the GLINK doorbell (and after
 * sending), every call does a bounded amount of work, and nothing here waits. A send that cannot go out yet (no remote intent, no FIFO
 * room) returns ModemGlinkAgain and the owner tries again on the next event; an intent is asked for once per pending send. */
#ifndef MODEM_GLINK_H
#define MODEM_GLINK_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MODEM_GLINK_MAX_CHANNELS 6u
#define MODEM_GLINK_MAX_REMOTE_INTENTS 48u   /* the audio DSP offers 24 small intents on adsp_apps before granting a larger one */
#define MODEM_GLINK_MAX_LOCAL_INTENTS 24u
#define MODEM_GLINK_NAME 32u
#define MODEM_GLINK_FEATURE_INTENT_REUSE 1u

enum {
    MODEM_GLINK_VERSION = 0, MODEM_GLINK_VERSION_ACK = 1, MODEM_GLINK_OPEN = 2, MODEM_GLINK_CLOSE = 3, MODEM_GLINK_OPEN_ACK = 4,
    MODEM_GLINK_INTENT = 5, MODEM_GLINK_RX_DONE = 6, MODEM_GLINK_RX_INTENT_REQ = 7, MODEM_GLINK_RX_INTENT_REQ_ACK = 8,
    MODEM_GLINK_TX_DATA = 9, MODEM_GLINK_CLOSE_ACK = 11, MODEM_GLINK_TX_DATA_CONT = 12, MODEM_GLINK_READ_NOTIF = 13,
    MODEM_GLINK_RX_DONE_W_REUSE = 14, MODEM_GLINK_SIGNALS = 15,
};

enum {
    ModemGlinkOk = 0,
    ModemGlinkAgain = -1,    /* not now: no intent or no FIFO room yet */
    ModemGlinkNotOpen = -2,
    ModemGlinkTooBig = -3,
    ModemGlinkFull = -4,     /* no channel slot */
    ModemGlinkCorrupt = -5,  /* the receive FIFO holds something that cannot be a message: the link is stopped */
    ModemGlinkArgument = -6,
};

/* One direction's FIFO in SMEM: indices are byte offsets into data. */
typedef struct {
    volatile uint32_t *head, *tail;
    volatile uint8_t *data;
    uint32_t length;
} ModemGlinkFifo;

typedef struct ModemGlinkChannel ModemGlinkChannel;
typedef struct {
    void (*state)(void *context, ModemGlinkChannel *channel, int up);
    /* A complete message; data stays valid for the duration of the call only. */
    void (*receive)(void *context, ModemGlinkChannel *channel, const uint8_t *data, uint32_t length);
} ModemGlinkClient;

struct ModemGlinkChannel {
    char name[MODEM_GLINK_NAME];
    uint16_t lcid, rcid;
    uint8_t used, initiator, open_sent, open_acked, remote_open, up, intent_asked;
    ModemGlinkClient client;
    void *context;
    /* Our receive intents: count buffers of size bytes in storage (count * size). */
    uint8_t *storage;
    uint32_t intent_size, intent_count;
    struct {uint32_t fill; uint8_t advertised, busy;} local[MODEM_GLINK_MAX_LOCAL_INTENTS];
    /* The remote's intents we may send into. */
    struct {uint32_t id, size; uint8_t valid, busy;} remote[MODEM_GLINK_MAX_REMOTE_INTENTS];
};

typedef struct {
    ModemGlinkFifo tx, rx;
    void (*kick)(void *context);
    void *kick_context;
    uint32_t features;
    uint8_t version_sent, version_agreed, broken, kick_pending, notify_sent;
    uint16_t next_lcid;
    ModemGlinkChannel channel[MODEM_GLINK_MAX_CHANNELS];
    /* Counters for the log. */
    uint32_t rx_messages, tx_messages, rx_data, unknown_opens, dropped;
    /* How many times a send found no room: the remote's intents are all held, or all of them are too small.
       The two are counted apart, because only the first is a credit that can run out and be given back. */
    uint32_t no_intent, intent_too_small, intent_requested;
    /* How many times the remote told us an intent is free again. The modem answers our requests by reading
       them, and it has to give the intent back: without that the credit is spent once and never returns. */
    uint32_t rx_done;
    /* How many of the remote's commands were shown in the log. The dump is limited so a busy link cannot
       flood the log. */
    uint32_t traced;
    char last_unknown[MODEM_GLINK_NAME];
    char unknown_names[96];  /* the distinct names of those opens, space separated, as many as fit */
} ModemGlink;

/* Sets up the link on its FIFOs (rx may have no data yet: the remote makes its FIFO later) and resets the indices we own. */
void modem_glink_init(ModemGlink *g, ModemGlinkFifo tx, ModemGlinkFifo rx, void (*kick)(void *), void *kick_context);
/* The remote's FIFO once it exists. */
void modem_glink_set_rx(ModemGlink *g, volatile uint8_t *data, uint32_t length);
/* Registers a channel by name with intent_count receive buffers of intent_size bytes in storage. open_now: we send OPEN ourselves;
 * otherwise we answer when the remote opens it. */
ModemGlinkChannel *modem_glink_channel(ModemGlink *g, const char *name, uint32_t intent_size, uint32_t intent_count, uint8_t *storage,
                                     const ModemGlinkClient *client, void *context, int open_now);
/* Sends VERSION (once) and any channel OPEN that is due, then handles every complete message in the receive FIFO. Returns the number of
 * messages handled or ModemGlinkCorrupt. Rings the remote when anything was written. */
int modem_glink_poll(ModemGlink *g);
/* Queues one message on an open channel (single chunk, up to the remote intent size). */
int modem_glink_send(ModemGlink *g, ModemGlinkChannel *channel, const void *data, uint32_t length);

/* FIFO primitives, exposed for the tests. */
uint32_t modem_glink_rx_avail(const ModemGlinkFifo *f);
uint32_t modem_glink_tx_avail(const ModemGlinkFifo *f);

#ifdef __cplusplus
}
#endif
#endif
