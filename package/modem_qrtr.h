/* fuxi 2026-11-26: from ljhhkx529/QcomBaseband (qcom_qrtr.h), the modem GLINK/QRTR renamed modem_glink_/modem_qrtr_. */
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * QRTR over the fuxi's GLINK-SMEM link: the wire protocol, the node and port bookkeeping and the name service.
 *
 * The fuxi's modem is a Qualcomm MPSS inside the SoC, not an Apple PCIe baseband. It is reached over GLINK (two SMEM
 * channels: IPCRTR for the QRTR traffic and DS for the data services), and the QRTR protocol runs over IPCRTR.
 *
 * Wire format (Linux net/qrtr/af_qrtr.c, qrtr_hdr_v1): a 32-byte little-endian header of
 *
 *   version(4) type(4) src_node(4) src_port(4) confirm_rx(4) size(4) dst_node(4) dst_port(4)
 *
 * followed by `size` bytes of payload, padded to a 4-byte boundary. The version is 1. The type is one of QRTR_TYPE_*.
 * The control port is 0xfffffffe; the broadcast node is 0xffffffff.
 *
 * The name service: the modem announces its services with NEW_SERVER (service, instance, node, port) and takes them
 * away with DEL_SERVER. A client opens a port on the modem's node and sends a DATA packet; the modem answers on the
 * same port.
 *
 * The node and port rules, from Linux net/qrtr/af_qrtr.c and ns.c and from the working Trogdor reference port:
 *
 *   - the AP's own node id is 1 (Linux qrtr_local_nid, Trogdor's WQ_NODE). It is what the modem addresses us by.
 *   - a control packet (HELLO, NEW_SERVER, DEL_SERVER, DEL_CLIENT) always carries the control port 0xfffffffe as its
 *     source port, and is 20 bytes: the command and four words.
 *   - the first HELLO goes to the broadcast node 0xffffffff, because the modem's node id is not known yet; the reply
 *     HELLO is addressed to the node id the modem's own HELLO carried.
 *   - a DATA packet carries the local port the client bound (the modem routes its answer back by it) as its source
 *     port, and the service's node and port as its destination.
 *   - a DEL_CLIENT tells the peer that a local port is gone; it goes to the broadcast node.
 *
 * The HELLO race: Linux flushes the delayed say_hello work when a reply arrives before our own HELLO went out
 * (net/qrtr/af_qrtr.c, the qcom fuxi patch 0005-qrtr.patch). Without that, every reply to the first packet is dropped
 * and no service is ever seen. This module sends HELLO as soon as the link is up and accepts replies at any time.
 */
#ifndef MODEM_QRTR_H
#define MODEM_QRTR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* qrtr_hdr_v1 */
#define QRTR_PROTO_VER_1 1u
#define QRTR_HDR_LEN 32u

#define QRTR_NODE_BCAST 0xffffffffu
#define QRTR_PORT_CTRL 0xfffffffeu

enum {
    QRTR_TYPE_DATA = 1,
    QRTR_TYPE_HELLO = 2,
    QRTR_TYPE_BYE = 3,
    QRTR_TYPE_NEW_SERVER = 4,
    QRTR_TYPE_DEL_SERVER = 5,
    QRTR_TYPE_DEL_CLIENT = 6,
    QRTR_TYPE_RESUME_TX = 7,
    QRTR_TYPE_EXIT = 8,
    QRTR_TYPE_PING = 9,
    QRTR_TYPE_NEW_LOOKUP = 10,
    QRTR_TYPE_DEL_LOOKUP = 11,
};

/* struct qrtr_ctrl_pkt: cmd then the union of server and client */
#define QRTR_CTRL_LEN 20u

/* The node id the AP claims. Linux qrtr_local_nid is 1 and the Trogdor reference port uses the same; the modem
   addresses the AP by it. */
#define QRTR_LOCAL_NODE 1u

/* The flow control watermarks of upstream's net/qrtr/af_qrtr.c: QRTR_TX_FLOW_LOW and QRTR_TX_FLOW_HIGH. The
   low watermark is the number of outstanding DATA packets at which the confirm_rx flag is set; the high
   watermark is the number at which the sender must stop. */
#define QRTR_TX_FLOW_LOW 5u
#define QRTR_TX_FLOW_HIGH 10u

/* How many destinations are tracked separately. Upstream keys the flow on the destination's node and port
   (Linux net/qrtr/af_qrtr.c, qrtr_tx_wait: `key = dest_node << 32 | dest_port`).

   The modem opens a new source port for every stream it starts, so the destinations come and go: the seven
   RMTFS requests of 2026-11-22 came from seven ports. The table is therefore larger than the number of ports
   the modem was seen to use, and a slot whose credit is all back is reused rather than the packet refused. */
#define QRTR_MAX_FLOWS 48u

#define QRTR_MAX_SERVERS 64u
/* The largest packet the layer assembles: the header plus a QMI SDU. */
#define QRTR_MAX_PACKET 8192u

/* One service the modem announced. */
struct qrtr_server {
    uint32_t service, instance, node, port;
    uint8_t present;
};

/* The link the protocol layer writes through. Returns 0 when the bytes are on their way. The source port says which
   local port the packet comes from: the control port for a control packet, the client's own port for a DATA packet,
   because the modem routes its answer back by it. */
typedef int (*qrtr_send_fn)(void *context, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *data,
                            uint32_t length);
typedef void (*qrtr_server_fn)(void *context, uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port);
/* Hands a DATA packet's payload to the bridge. The destination port is the local client port the answer belongs to,
   the source node and port say who sent it. Implemented by the board adapter. */
typedef void (*qrtr_data_fn)(void *context, uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body,
                             uint32_t length);
/* The peer asks which of our services it may have. The hook announces the ones that match and ends the
   listing with the empty NEW_SERVER. Upstream's name service answers a lookup the same way
   (Linux net/qrtr/ns.c, ctrl_cmd_new_lookup and lookup_notify). */
typedef void (*qrtr_lookup_fn)(void *context, uint32_t service, uint32_t version_instance, uint32_t node);

/* The transport's state. The board fills in the link hooks; the protocol layer keeps the rest. */
struct qrtr_state {
    uint32_t local_node;              /* the node id the AP claims; the modem's is learned from its HELLO */
    uint32_t remote_node;             /* the modem's node id, from its HELLO or its NEW_SERVER */
    /* The node and port the peer's HELLO body carried. Upstream's name service ignores the HELLO body
       (Linux net/qrtr/ns.c, ctrl_cmd_hello just answers with the local servers), but the modem puts a node
       and a port there that are not the ones its header names. They are kept so a lookup can be sent to
       them: a service the modem holds on that node is not reachable from the header's node. */
    uint32_t peer_node, peer_port;
    uint8_t peer_node_known;
    uint8_t remote_node_known, hello_sent, hello_received;
    /* How many HELLOs the peer has sent. The peer sends one again whenever it (re)opens its link, and a
       service this side offers is announced again on each of them: the peer learns of the service only from
       the announcement, so a HELLO that is not answered with one leaves the peer without the service. */
    uint32_t hellos;
    /* The reply to the peer's HELLO, owed when the link could not take it the first time. The link is busy more
       often than not right after the handshake: the remote's intent is still held by the packet that carried the
       HELLO. Without this the answer is lost and the peer never learns our node id. */
    uint8_t hello_reply_owed;
    uint32_t hello_reply_node, hello_reply_port;
    /* The reply's control body: the command word the peer reads and the node and port it looks for. */
    uint8_t hello_reply_body[QRTR_CTRL_LEN];
    struct qrtr_server server[QRTR_MAX_SERVERS];
    uint32_t servers;
    /* The link hooks. */
    qrtr_send_fn send;
    qrtr_server_fn server_fn;
    qrtr_data_fn data_fn;
    qrtr_lookup_fn lookup_fn;
    void *context;
    /* An optional hook that sees the control packets as they arrive, so the wire can be read off the log. */
    void (*trace)(void *context, const uint8_t *bytes, uint32_t length);
    void *trace_context;
    /* An optional hook that sees the packets as they go out. The receive-side trace shows what the peer sent;
       this one shows what we sent, which is the only way to tell a lookup the peer ignored from a lookup that
       never left. */
    void (*trace_tx)(void *context, const uint8_t *bytes, uint32_t length);
    void *trace_tx_context;
    /* How many packets the trace hook has seen. The dump is limited, so a busy link cannot flood the log. The
       control packets and the data packets have their own budgets: the data packets are what the modem answers
       in, and a run of data packets would otherwise use up the whole budget before the name service's state
       showed up. */
    uint32_t traced;
    uint32_t traced_data;
    /* The same budget for the transmit-side trace. */
    uint32_t traced_tx;
    uint32_t traced_tx_data;
    /* How many lookups the peer has answered. The end of a lookup listing is an empty NEW_SERVER, so this counts
       the listings that are complete. */
    uint32_t lookups_answered;
    /* The flow control of upstream's net/qrtr/af_qrtr.c: a DATA packet that crosses the low watermark carries
       the confirm_rx flag, and the peer answers it with RESUME_TX. Without this the peer stops answering after
       its high watermark of outstanding packets, and the link goes quiet in both directions with nothing dropped
       on our side.

       The count is kept per destination, because upstream keys the flow on the destination's node and port
       (qrtr_tx_wait: `key = dest_node << 32 | dest_port`) and a RESUME_TX names the destination it frees. */
    struct qrtr_flow {
        uint32_t node, port;
        uint32_t pending;   /* the DATA packets the peer has not credited yet */
        uint8_t used;
    } flow[QRTR_MAX_FLOWS];
    uint32_t resumes_sent, resumes_received;
    /* Counters for the log. */
    uint32_t tx, rx, dropped, malformed, hello_dropped;
    char last_error[96];
    /* The scratch the header and its payload are assembled in, so they go out as one buffer. */
    uint8_t scratch[QRTR_MAX_PACKET];
};

/* Resets the protocol state and sends HELLO. */
void qrtr_init(struct qrtr_state *q, uint32_t local_node, qrtr_send_fn send, qrtr_server_fn server,
               void *context);
/* Sets the handler for DATA packets. */
void qrtr_set_data_handler(struct qrtr_state *q, qrtr_data_fn data);
/* Sets the handler for the peer's lookup: it announces the services of ours that match. */
void qrtr_set_lookup_handler(struct qrtr_state *q, qrtr_lookup_fn lookup);
/* Sets the hook that sees the first packets as they arrive. */
void qrtr_set_trace(struct qrtr_state *q, void (*trace)(void *context, const uint8_t *bytes, uint32_t length),
                    void *context);
/* Sets the hook that sees the packets as they go out. */
void qrtr_set_trace_tx(struct qrtr_state *q, void (*trace)(void *context, const uint8_t *bytes, uint32_t length),
                       void *context);
/* Sends one QRTR packet: the 32-byte header and the payload. The source port is the control port. */
int qrtr_send(struct qrtr_state *q, uint32_t dst_node, uint32_t dst_port, uint32_t type, const void *body, uint32_t length);
/* Sends one QRTR packet from a given source port. */
int qrtr_send_from(struct qrtr_state *q, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, uint32_t type, const void *body,
                   uint32_t length);
/* Sends a DATA packet to a node and port, from the local port the client bound. */
int qrtr_send_data(struct qrtr_state *q, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *body, uint32_t length);
/* Tells the peer that a local client port is gone. */
int qrtr_send_del_client(struct qrtr_state *q, uint32_t local_port);
/* Announces one of our own services to the peer: a NEW_SERVER to the peer's control port, naming the
 * service, the version and instance it is published under, and the local port it is served on. */
int qrtr_send_new_server(struct qrtr_state *q, uint32_t dst_node, uint32_t service, uint32_t version, uint32_t instance,
                         uint32_t port);
/* Asks the peer which services it has.
 *
 * The peer answers with NEW_SERVER for every service that matches and with an empty NEW_SERVER at the end of the
 * list (Linux net/qrtr/ns.c, ctrl_cmd_new_lookup and lookup_notify). A service of 0 and an instance of 0 match
 * anything, so that one lookup asks for the whole list (Linux ns.c: `if (lookup->service && lookup->service !=
 * service) continue;`). The instance word holds the version in its low byte and the instance in its high byte
 * (Linux drivers/soc/qcom/qmi_interface.c, qmi_send_new_lookup). */
int qrtr_send_lookup(struct qrtr_state *q, uint32_t service, uint32_t version, uint32_t instance);
/* The same lookup, sent to a named node. A peer can hold services on a node other than the one its header
   names, and they are only reachable by asking that node. */
int qrtr_send_lookup_to(struct qrtr_state *q, uint32_t node, uint32_t service, uint32_t version, uint32_t instance);
/* Sends the reply to the peer's HELLO if it is still owed. The owner calls this whenever the link may have room
   again. Returns 1 when the reply went out. */
int qrtr_flush_hello_reply(struct qrtr_state *q);
/* The flow control of upstream's net/qrtr/af_qrtr.c. The peer stops answering once its high watermark of
   outstanding DATA packets is reached, and says so with RESUME_TX; the peer asks for the same when our packet
   carries the confirm_rx flag. */
uint32_t qrtr_flow_pending(const struct qrtr_state *q, uint32_t node, uint32_t port);
int qrtr_resume_tx(struct qrtr_state *q, uint32_t node, uint32_t port);
int qrtr_send_resume_tx(struct qrtr_state *q, uint32_t peer_node, uint32_t peer_port, uint32_t our_node, uint32_t our_port);
/* Hands one received QRTR packet (header and payload) to the protocol layer. */
void qrtr_receive(struct qrtr_state *q, const void *data, uint32_t length);
/* The service the modem announced with the lowest instance, or 0. */
uint32_t qrtr_lookup(struct qrtr_state *q, uint32_t service);

#ifdef __cplusplus
}
#endif
#endif
