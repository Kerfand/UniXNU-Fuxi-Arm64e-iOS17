/* fuxi 2026-11-26: from ljhhkx529/QcomBaseband (qcom_qrtr.c), the modem GLINK/QRTR renamed modem_glink_/modem_qrtr_. */
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The QRTR protocol layer over the fuxi's GLINK-SMEM link: the 32-byte qrtr_hdr_v1 framing, the control packets and
 * the name service. Portable: no kernel types beyond the fixed-width integers, so the model test can run it on the host.
 */
#include "modem_qrtr.h"

#include <string.h>

/* The diagnostics below are kernel-only: the host model tests include this file directly and bring their own
   stdio, so the header stays under the same guard the kernel build sets. */
/* The portable layer has no kernel headers in its include path; the two symbols the diagnostics need are declared
   here so the kernel build sees them and the host tests stay on their own stdio. */
#ifdef KERNEL
extern int printf(const char *fmt, ...);
extern int snprintf(char *buf, unsigned long len, const char *fmt, ...);
#endif

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Whether a packet type is one of the name service's control packets. */
static int type_is_control(uint32_t type)
{
    switch (type) {
    case QRTR_TYPE_HELLO:
    case QRTR_TYPE_BYE:
    case QRTR_TYPE_NEW_SERVER:
    case QRTR_TYPE_DEL_SERVER:
    case QRTR_TYPE_DEL_CLIENT:
    case QRTR_TYPE_RESUME_TX:
    case QRTR_TYPE_EXIT:
    case QRTR_TYPE_PING:
    case QRTR_TYPE_NEW_LOOKUP:
    case QRTR_TYPE_DEL_LOOKUP:
        return 1;
    default:
        return 0;
    }
}

static struct qrtr_server *server_find(struct qrtr_state *q, uint32_t service, uint32_t instance){
    /* The whole array is walked, not the first `servers` entries: a removed server leaves a hole. */
    for (uint32_t i = 0; i < QRTR_MAX_SERVERS; i++)
        if (q->server[i].present && q->server[i].service == service && q->server[i].instance == instance)
            return &q->server[i];
    return 0;
}

static struct qrtr_server *server_add(struct qrtr_state *q, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    /* A service of 0 or a port of 0 is refused (Linux net/qrtr/ns.c, server_add: `if (!service || !port) return
       NULL;`). The empty NEW_SERVER that ends a lookup listing carries service 0, so it is not a service and must
       not be added as one. */
    if (!service || !port) return 0;
    struct qrtr_server *s = server_find(q, service, instance);
    if (s) {
        s->node = node;
        s->port = port;
        return s;
    }
    for (uint32_t i = 0; i < QRTR_MAX_SERVERS; i++) {
        if (q->server[i].present) continue;
        q->server[i].service = service;
        q->server[i].instance = instance;
        q->server[i].node = node;
        q->server[i].port = port;
        q->server[i].present = 1;
        q->servers++;
        return &q->server[i];
    }
    return 0;
}

static void server_remove(struct qrtr_state *q, uint32_t service, uint32_t instance)
{
    struct qrtr_server *s = server_find(q, service, instance);
    if (!s) return;
    s->present = 0;
    q->servers--;
}

void qrtr_init(struct qrtr_state *q, uint32_t local_node, qrtr_send_fn send, qrtr_server_fn server, void *context)
{
    memset(q, 0, sizeof *q);
    q->local_node = local_node;
    q->send = send;
    q->server_fn = server;
    q->context = context;
}

void qrtr_set_data_handler(struct qrtr_state *q, qrtr_data_fn data)
{
    q->data_fn = data;
}

void qrtr_set_lookup_handler(struct qrtr_state *q, qrtr_lookup_fn lookup)
{
    q->lookup_fn = lookup;
}

void qrtr_set_trace(struct qrtr_state *q, void (*trace)(void *context, const uint8_t *bytes, uint32_t length),
                    void *context)
{
    q->trace = trace;
    q->trace_context = context;
}

void qrtr_set_trace_tx(struct qrtr_state *q, void (*trace)(void *context, const uint8_t *bytes, uint32_t length),
                       void *context)
{
    q->trace_tx = trace;
    q->trace_tx_context = context;
}

int qrtr_send(struct qrtr_state *q, uint32_t dst_node, uint32_t dst_port, uint32_t type, const void *body, uint32_t length)
{
    return qrtr_send_from(q, QRTR_PORT_CTRL, dst_node, dst_port, type, body, length);
}

int qrtr_send_data(struct qrtr_state *q, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *body, uint32_t length)
{
    return qrtr_send_from(q, src_port, dst_node, dst_port, QRTR_TYPE_DATA, body, length);
}

int qrtr_send_del_client(struct qrtr_state *q, uint32_t local_port)
{
    uint8_t body[QRTR_CTRL_LEN];
    wr32(body + 0, QRTR_TYPE_DEL_CLIENT);
    wr32(body + 4, q->local_node);
    wr32(body + 8, local_port);
    /* The peer is told over the broadcast node, as Linux's qrtr_port_remove does. */
    return qrtr_send_from(q, QRTR_PORT_CTRL, QRTR_NODE_BCAST, QRTR_PORT_CTRL, QRTR_TYPE_DEL_CLIENT, body, sizeof body);
}

/* Builds the control body a control packet carries.
 *
 * Upstream's name service does not read the header's type field: it reads the command word at the start of
 * the body (Linux net/qrtr/ns.c, the qrtr_ns_worker switch on pkt->cmd) and then the body is a whole
 * qrtr_ctrl_pkt: the command word and a 16-byte union (Linux include/uapi/linux/qrtr.h). The modem follows
 * that convention: its own HELLO carries a 20-byte body whose first word is the command, as the bytes the
 * device sent on 2026-11-20 showed (header type 2, body 20 bytes, body's first word 2).
 *
 * So every control packet this port sends has to carry that body. A control packet without one is
 * invisible to the peer: it has no command word to read, and upstream's own receive path rejects a
 * zero-size packet outright (af_qrtr.c: if (!size || size > len ...) goto err).
 */
static void build_ctrl_body(uint8_t *body, uint32_t cmd, uint32_t node, uint32_t port)
{
    memset(body, 0, QRTR_CTRL_LEN);
    wr32(body + 0, cmd);
    wr32(body + 4, node);
    wr32(body + 8, port);
}

/* Announces one of our own services to the peer.
 *
 * The body is a whole qrtr_ctrl_pkt: the command word the peer switches on, and then the service, the
 * instance word, our node and the local port the service is served on. The instance word holds the version in
 * its low byte and the instance in its high byte (the same word the lookup uses, Linux drivers/soc/qcom/
 * qmi_interface.c, qmi_send_new_lookup), so version 1 and instance 1 are the word 0x0101.
 *
 * The peer reads the command word out of the body and not out of the header's type field, the same way it
 * reads the HELLO's. */
int qrtr_send_new_server(struct qrtr_state *q, uint32_t dst_node, uint32_t service, uint32_t version, uint32_t instance,
                         uint32_t port)
{
    uint8_t body[QRTR_CTRL_LEN];
    memset(body, 0, sizeof body);
    wr32(body + 0, QRTR_TYPE_NEW_SERVER);
    wr32(body + 4, service);
    wr32(body + 8, (version & 0xffu) | ((instance & 0xffu) << 8));
    wr32(body + 12, q->local_node);
    wr32(body + 16, port);
    return qrtr_send(q, dst_node, QRTR_PORT_CTRL, QRTR_TYPE_NEW_SERVER, body, sizeof body);
}

int qrtr_send_lookup(struct qrtr_state *q, uint32_t service, uint32_t version, uint32_t instance)
{
    uint8_t body[QRTR_CTRL_LEN];
    /* The body is a whole qrtr_ctrl_pkt: the command word and the server union. The peer reads the command word
       and then the service and the instance out of the union (Linux net/qrtr/ns.c, ctrl_cmd_new_lookup). */
    memset(body, 0, sizeof body);
    wr32(body + 0, QRTR_TYPE_NEW_LOOKUP);
    wr32(body + 4, service);
    /* The instance word holds the version in its low byte and the instance in its high byte
       (Linux drivers/soc/qcom/qmi_interface.c, qmi_send_new_lookup). */
    wr32(body + 8, (version & 0xffu) | ((instance & 0xffu) << 8));
    /* The lookup goes to the control port of the node we know: the modem's node id if its HELLO has arrived,
       otherwise the broadcast node. The peer answers on the control port. */
    const uint32_t node = q->remote_node_known ? q->remote_node : QRTR_NODE_BCAST;
    return qrtr_send(q, node, QRTR_PORT_CTRL, QRTR_TYPE_NEW_LOOKUP, body, sizeof body);
}

int qrtr_send_lookup_to(struct qrtr_state *q, uint32_t node, uint32_t service, uint32_t version, uint32_t instance)
{
    uint8_t body[QRTR_CTRL_LEN];
    memset(body, 0, sizeof body);
    wr32(body + 0, QRTR_TYPE_NEW_LOOKUP);
    wr32(body + 4, service);
    wr32(body + 8, (version & 0xffu) | ((instance & 0xffu) << 8));
    return qrtr_send(q, node, QRTR_PORT_CTRL, QRTR_TYPE_NEW_LOOKUP, body, sizeof body);
}

/* The flow of one destination: the DATA packets the peer has not credited yet. Upstream keys the flow on the
   destination's node and port (Linux net/qrtr/af_qrtr.c, qrtr_tx_wait: `key = dest_node << 32 | dest_port`).

   The table is small and the destinations are not: on 2026-11-22 the modem sent its seven RMTFS requests from
   seven different source ports, and it sends the bridge's own requests from others, so the table filled up with
   one slot per port and the eighth destination's answer was refused with nowhere on the log to say why.

   A slot whose credit is all back is free in every sense that matters: it holds no outstanding packet, and it is
   made again on demand with the same zero count. So a full table takes one of those over rather than refusing
   the packet. */
/* Which of the two refusals happened, and what the flow table looks like when it did: the refusal path and the
   credit path return the same -1, and the table being full is what stops a destination from ever getting a flow.
   One line per destination and reason, then each power of two, plus a dump of all the slots at most once per
   ten seconds; the counters keep the totals. */
#ifdef KERNEL
static void qrtr_send_why(struct qrtr_state *q, const char *why, uint32_t node, uint32_t port, uint32_t pending)
{
  struct { uint32_t key, count; } static seen[16];
  static uint32_t dumps;
  const uint32_t key = ((uint32_t)(why[0] == 't') << 31) | (node & 0xffffu) << 16 | (port & 0xffffu);
  uint32_t count = 0;
  for (uint32_t i = 0; i < 16u; i++) {
    if (seen[i].key != key && seen[i].key) continue;
    seen[i].key = key;
    count = ++seen[i].count;
    break;
  }
  if (count == 0u || (count > 2u && (count & (count - 1u)))) return;
  printf("COACHZ qrtr: %s node %u port %u pending %u (%u)\n", why, node, port, pending, count);
  if (why[0] == 't' && (++dumps <= 2u || !(dumps & (dumps - 1u)))) {
    char slots[QRTR_MAX_FLOWS * 26u];
    uint32_t at = 0;
    for (uint32_t i = 0; i < QRTR_MAX_FLOWS; i++) {
      const struct qrtr_flow *fl = &q->flow[i];
      at += (uint32_t)snprintf(slots + at, sizeof slots - at, "%u%s%u:%u.%u ", i, fl->used ? ":" : "-", fl->node, fl->port, fl->pending);
      if (at + 30u >= sizeof slots) break;
    }
    slots[at < sizeof slots ? at : sizeof slots - 1u] = '\0';
    printf("COACHZ qrtr: flow table: %s\n", slots);
  }
}
#else
static void qrtr_send_why(struct qrtr_state *q, const char *why, uint32_t node, uint32_t port, uint32_t pending)
{
  (void)q; (void)why; (void)node; (void)port; (void)pending;
}
#endif

/* The three stages of the TX confirm handshake, one line each and only when a destination is being watched:
   confirm_set on the DATA that crosses the low watermark, confirm_sent when that DATA actually leaves (a refused
   send or a retried hold is not a send), and resume_rx when the peer frees the destination. The node and port say
   which destination, the pending value says where the credit stood. */
static void qrtr_confirm_note(const char *stage, uint32_t node, uint32_t port, uint32_t pending, uint32_t src_port, uint32_t matched)
{
#ifdef KERNEL
  printf("COACHZ qrtr_confirm: %s node %u port %u pending %u src %u matched %u\n", stage, node, port, pending, src_port, matched);
#else
  (void)stage; (void)node; (void)port; (void)pending; (void)src_port; (void)matched;
#endif
}

static struct qrtr_flow *flow_find(struct qrtr_state *q, uint32_t node, uint32_t port, int create)
{
    struct qrtr_flow *free_slot = 0;
    struct qrtr_flow *settled_slot = 0;
    for (uint32_t i = 0; i < QRTR_MAX_FLOWS; i++) {
        struct qrtr_flow *f = &q->flow[i];
        if (!f->used) {
            if (!free_slot) free_slot = f;
            continue;
        }
        if (f->node == node && f->port == port) return f;
        /* A destination whose credit is all back: nothing of its is outstanding. */
        if (!settled_slot && !f->pending) settled_slot = f;
    }
    if (!create) return 0;
    if (!free_slot) free_slot = settled_slot;
    if (!free_slot) return 0;
    free_slot->used = 1;
    free_slot->node = node;
    free_slot->port = port;
    free_slot->pending = 0;
    return free_slot;
}

/* The peer freed the credit for one destination. The RESUME_TX body names that destination, and the count for it
   starts over (Linux net/qrtr/af_qrtr.c, qrtr_tx_resume: `flow->pending = 0`). */
/* Diagnostic: what the destination's flow holds, or ~0 when it has none. The adapter's held-packet retry reports
   the confirm it sends, and the pending value belongs to the flow table here. */
uint32_t qrtr_flow_pending(const struct qrtr_state *q, uint32_t node, uint32_t port)
{
    for (uint32_t i = 0; i < QRTR_MAX_FLOWS; i++)
        if (q->flow[i].used && q->flow[i].node == node && q->flow[i].port == port) return q->flow[i].pending;
    return 0xffffffffu;
}

int qrtr_resume_tx(struct qrtr_state *q, uint32_t node, uint32_t port)
{
    struct qrtr_flow *f = flow_find(q, node, port, 0);
#ifdef KERNEL
    const uint32_t before = f ? f->pending : 0u;
#endif
    if (f) f->pending = 0;
    q->resumes_received++;
#ifdef KERNEL
    printf("COACHZ qrtr: resume node %u port %u matched %u pending before %u\n", node, port, f ? 1u : 0u, before);
    qrtr_confirm_note("resume_rx", node, port, before, 0u, f ? 1u : 0u);
#endif
    return 1;
}

/* Our credit is back: the peer asked for a RESUME_TX, so we tell it we may receive again.

   Upstream sends the RESUME_TX from the port the flagged packet was addressed to, to the sender's own node and
   port, and the body carries the destination of the flagged packet (Linux net/qrtr/af_qrtr.c,
   qrtr_send_resume_tx: `pkt->client.node = cb->dst_node; pkt->client.port = cb->dst_port`, and the packet goes
   from the local address to the remote one). The sender frees its flow by that body, so the body has to name the
   destination the sender addressed its packet to.

   peer_node and peer_port are where the packet came from; our_node and our_port are where it was addressed. */
int qrtr_send_resume_tx(struct qrtr_state *q, uint32_t peer_node, uint32_t peer_port, uint32_t our_node, uint32_t our_port)
{
    uint8_t body[QRTR_CTRL_LEN];
    memset(body, 0, sizeof body);
    wr32(body + 0, QRTR_TYPE_RESUME_TX);
    wr32(body + 4, our_node);
    wr32(body + 8, our_port);
    const int result = qrtr_send_from(q, our_port, peer_node, peer_port, QRTR_TYPE_RESUME_TX, body, sizeof body);
    if (result == 0) q->resumes_sent++;
    return result;
}

int qrtr_flush_hello_reply(struct qrtr_state *q)
{
    const int result = qrtr_send(q, q->hello_reply_node, q->hello_reply_port, QRTR_TYPE_HELLO, q->hello_reply_body,
                                 QRTR_CTRL_LEN);
    if (result == 0) {
        q->hello_reply_owed = 0;
        return 1;
    }
    /* Still owed: the link had no room. */
    q->hello_dropped++;
    return 0;
}

int qrtr_send_from(struct qrtr_state *q, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, uint32_t type,
                   const void *body, uint32_t length)
{
    if (!q->send) return -1;
    /* A DATA packet may not go out before our own HELLO: the modem's name service ignores it. A reply that arrives
       before our HELLO did is the race Linux fixes by flushing the say_hello work (the fuxi patch 0005-qrtr.patch);
       here the HELLO is sent as soon as the link is up, so only the ordering matters. */
    if (type != QRTR_TYPE_HELLO && !q->hello_sent) {
        q->hello_dropped++;
        return -1;
    }
    /* A control packet carries the command word the peer reads, in the body. A caller that hands over no body
       gets one built here, so a control packet cannot go out without the word the peer switches on. */
    uint8_t built[QRTR_CTRL_LEN];
    if (!body && type != QRTR_TYPE_DATA) {
        build_ctrl_body(built, type, q->local_node, src_port);
        body = built;
        length = QRTR_CTRL_LEN;
    }
    /* The header and the payload travel in one buffer, as Linux does: skb_push() puts the header in front of the
       payload the skb already holds, and skb_put_padto() rounds the payload up to four bytes. */
    const uint32_t padded = (length + 3u) & ~3u;
    if (QRTR_HDR_LEN + padded > sizeof q->scratch) {
        q->dropped++;
        return -1;
    }
    uint8_t *p = q->scratch;
    wr32(p + 0, QRTR_PROTO_VER_1);
    wr32(p + 4, type);
    wr32(p + 8, q->local_node);
    wr32(p + 12, src_port);
    /* The confirm_rx flag asks the peer for a RESUME_TX when it has room again. Upstream sets it on the DATA packet
       that crosses the low watermark (Linux net/qrtr/af_qrtr.c, qrtr_tx_wait), and the peer stops answering once
       its high watermark of outstanding packets is reached. */
    uint32_t confirm_rx = 0;
    struct qrtr_flow *flow = 0;
    if (type == QRTR_TYPE_DATA) {
        flow = flow_find(q, dst_node, dst_port, 1);
        if (!flow) {
            /* No room for another destination's flow: the packet is refused rather than sent unaccounted for. */
            q->dropped++;
            qrtr_send_why(q, "table full", dst_node, dst_port, 0u);
            return -1;
        }
        if (flow->pending >= QRTR_TX_FLOW_HIGH) {
            /* The peer is full. The packet waits for the RESUME_TX that frees the credit. */
            q->dropped++;
            qrtr_send_why(q, "credit high", dst_node, dst_port, flow->pending);
            return -1;
        }
        flow->pending++;
        confirm_rx = flow->pending == QRTR_TX_FLOW_LOW;
        if (confirm_rx) qrtr_confirm_note("confirm_set", dst_node, dst_port, flow->pending, src_port, 0u);
    }
    wr32(p + 16, confirm_rx);
    wr32(p + 20, length);
    wr32(p + 24, dst_node);
    wr32(p + 28, dst_port);
    if (length && body) memcpy(p + QRTR_HDR_LEN, body, length);
    for (uint32_t i = length; i < padded; i++) p[QRTR_HDR_LEN + i] = 0;
    /* The bytes that go out are shown before they are handed over: a lookup the peer ignored and a lookup that
       never left look the same from the receive side. */
    if (q->trace_tx) {
        if (type_is_control(type)) {
            if (q->traced_tx < 32) {
                q->traced_tx++;
                q->trace_tx(q->trace_tx_context, p, QRTR_HDR_LEN + padded);
            }
        } else if (q->traced_tx_data < 32) {
            q->traced_tx_data++;
            q->trace_tx(q->trace_tx_context, p, QRTR_HDR_LEN + padded);
        }
    }
    const int result = q->send(q->context, src_port, dst_node, dst_port, p, QRTR_HDR_LEN + padded);
    /* A held packet is retried in the adapter and reports itself there: only the send that returns 0 is a send. */
    if (confirm_rx && result == 0) qrtr_confirm_note("confirm_sent", dst_node, dst_port, flow ? flow->pending : 0u, src_port, 0u);
    if (result) {
        /* The packet did not go out, so it does not count against the peer's credit: the peer never saw it. */
        if (flow && flow->pending) flow->pending--;
        return result;
    }
    q->tx++;
    if (type == QRTR_TYPE_HELLO) q->hello_sent = 1;
    return 0;
}

void qrtr_receive(struct qrtr_state *q, const void *data, uint32_t length)
{
    const uint8_t *p = data;
    if (!p || length < QRTR_HDR_LEN) {
        q->malformed++;
        return;
    }
    q->rx++;
    /* Control packets are dumped in full, and data packets are not: the wire has to be readable off the log, and the
       control packets are the ones that carry the name service's state (which service, which instance, which node and
       port). The dump is limited to the first few control packets so a busy link cannot flood the log. The limit is
       32 because the startup sweep asks for several services by name and each answer is a control packet. */
    if (q->trace) {
        if (type_is_control(rd32(p + 4))) {
            if (q->traced < 32) {
                q->traced++;
                q->trace(q->trace_context, p, length);
            }
        } else if (q->traced_data < 32) {
            q->traced_data++;
            q->trace(q->trace_context, p, length);
        }
    }
    if (rd32(p + 0) != QRTR_PROTO_VER_1) {
        q->malformed++;
        return;
    }
    const uint32_t type = rd32(p + 4);
    const uint32_t src_node = rd32(p + 8);
    const uint32_t src_port = rd32(p + 12);
    const uint32_t size = rd32(p + 20);
    const uint32_t dst_node = rd32(p + 24);
    const uint32_t dst_port = rd32(p + 28);
    if (size > length - QRTR_HDR_LEN) {
        q->malformed++;
        return;
    }
    const uint8_t *body = p + QRTR_HDR_LEN;

    if (type == QRTR_TYPE_HELLO) {
        q->hello_received = 1;
        q->hellos++;
        q->remote_node = src_node;
        q->remote_node_known = 1;
        /* The peer's HELLO body carries a node and a port of its own, next to the command word. The header
           names a different node. Both are kept: the lookup is tried on each in turn, because a service the
           modem holds on the body's node is not reachable from the header's node. */
        if (size >= QRTR_CTRL_LEN) {
            q->peer_node = rd32(body + 4);
            q->peer_port = rd32(body + 8);
            q->peer_node_known = 1;
        }
        /* The modem may announce its services in the same breath as its HELLO; answer and carry on. The answer is
           owed until the link takes it: the remote's receive intent is still held by the very packet that carried
           the HELLO, so the first attempt is usually refused. */
        q->hello_reply_owed = 1;
        q->hello_reply_node = src_node;
        q->hello_reply_port = src_port;
        /* The answer carries the command word the peer reads, and the node and port it looks for: our own node id
           in the union's node field, and the control port. The modem's own HELLO carried the same shape. */
        build_ctrl_body(q->hello_reply_body, QRTR_TYPE_HELLO, q->local_node, QRTR_PORT_CTRL);
        qrtr_flush_hello_reply(q);
        return;
    }
    if (type == QRTR_TYPE_NEW_SERVER) {
        if (size < QRTR_CTRL_LEN) {
            q->malformed++;
            return;
        }
        const uint32_t service = rd32(body + 4);
        const uint32_t instance = rd32(body + 8);
        const uint32_t node = rd32(body + 12);
        const uint32_t port = rd32(body + 16);
        if (node) {
            q->remote_node = node;
            q->remote_node_known = 1;
        }
        /* The empty NEW_SERVER carries service 0 and port 0: it is the end of a lookup listing, not a service
           (Linux net/qrtr/ns.c, lookup_notify: "Empty notification, to indicate end of listing"). */
        if (!service || !port) {
            q->lookups_answered++;
            return;
        }
        /* A service the modem published is also an answer: the modem's name service answers a lookup with a
           NEW_SERVER for each service it holds, and the listing ends with the empty one. The counter used to
           move only for the empty one, so it stayed at zero on a modem that answers with services. */
        q->lookups_answered++;
        if (!server_add(q, service, instance, node, port)) {
            q->dropped++;
            return;
        }
        if (q->server_fn) q->server_fn(q->context, service, instance, node, port);
        return;
    }
    if (type == QRTR_TYPE_DEL_SERVER) {
        if (size < QRTR_CTRL_LEN) {
            q->malformed++;
            return;
        }
        server_remove(q, rd32(body + 4), rd32(body + 8));
        return;
    }
    if (type == QRTR_TYPE_RESUME_TX) {
        /* The peer says it may receive again. Its body names the destination whose credit is freed. */
        if (size >= QRTR_CTRL_LEN) qrtr_resume_tx(q, rd32(body + 4), rd32(body + 8));
        else q->malformed++;
        return;
    }
    if (type == QRTR_TYPE_DATA) {
        /* The peer asked for a RESUME_TX when it gets this packet, so we tell it our side is free again. The
           body names where we addressed the packet, which is the destination the sender has to free. */
        if (rd32(p + 16)) qrtr_send_resume_tx(q, src_node, src_port, dst_node, dst_port);
        /* The bridge's own hook takes the payload. The destination port is the local client port the answer belongs
           to; the source node and port say who sent it. */
        if (q->data_fn) q->data_fn(q->context, dst_port, src_node, src_port, body, size);
        else q->dropped++;
        return;
    }
    if (type == QRTR_TYPE_NEW_LOOKUP) {
        /* The peer asks which of our services it may have. The body carries the service and the instance
           word it wants, and a service of 0 and an instance of 0 ask for everything.
           Upstream's name service answers a lookup the same way (Linux net/qrtr/ns.c, ctrl_cmd_new_lookup
           and lookup_notify): a NEW_SERVER for every service that matches, and then the empty NEW_SERVER
           that ends the listing. */
        if (size < QRTR_CTRL_LEN) {
            q->malformed++;
            return;
        }
        const uint32_t service = rd32(body + 4);
        const uint32_t instance = rd32(body + 8);
        if (q->lookup_fn) q->lookup_fn(q->context, service, instance, src_node);
        return;
    }
    /* BYE, DEL_CLIENT, PING and the lookups need no answer here. */
    (void)dst_node;
}

uint32_t qrtr_lookup(struct qrtr_state *q, uint32_t service)
{
    /* The lowest instance of a service is the one a client takes, as qmicli's QRTR backend does. The whole array is
       walked, not the first `servers` entries: a removed server leaves a hole. */
    uint32_t best = 0xffffffffu, found = 0;
    for (uint32_t i = 0; i < QRTR_MAX_SERVERS; i++) {
        if (!q->server[i].present || q->server[i].service != service) continue;
        if (q->server[i].instance < best) {
            best = q->server[i].instance;
            found = q->server[i].port;
        }
    }
    return found;
}
