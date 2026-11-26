/* fuxi 2026-11-26: from ljhhkx529/QcomBaseband (fuxi_tftp.h), the modem GLINK/QRTR renamed modem_glink_/modem_qrtr_. */
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * TFTP: the modem's carrier configuration, fetched over QRTR.
 *
 * The modem reads its carrier configuration (the modem_pr and mcfg files) over a TFTP service, and stays
 * offline without it: the reference says so in one line (hakios.mpss.c, tftp: "the modem reads its carrier
 * configuration here and stays offline without it"), and the device agrees — on 2026-11-22 the modem's
 * operating mode was 5, PERSIST, and it had no registration and no signal.
 *
 * This is the second of the two gates the modem's stack has, and they are separate: RMTFS decides whether
 * the modem announces its stack at all (that one is done — the modem publishes forty-eight services), and
 * TFTP decides whether it goes from offline to online. The two were once mistaken for one, and the
 * mistake cost a round.
 *
 * The service is number 4096, published at two versions, 1 and 3, because the modem looks up the version
 * its own firmware expects (the reference publishes both, at ports 0x4002 and 0x4003).
 *
 * The protocol is TFTP's, over a QRTR data packet. The wire shape is:
 *
 *   bytes 0-1  the opcode, big-endian
 *   then       the path, NUL-terminated, the mode string, NUL-terminated, and the options as pairs of
 *              NUL-terminated name and value
 *
 * and the opcodes are the TFTP ones: RRQ 1, WRQ 2, DATA 3, ACK 4, ERROR 5, OACK 6.
 *
 * A request carries no block number: the path starts at byte 2. That is what the Linux port reads
 * (hakios.mpss.c:4311-4312: `name = (const char *)m + 2`), and the device confirms it: on 2026-11-22 the
 * modem's own requests came in as `00 01 2f 72 65 61 64 ...`, and the path is "/readonly/..." from the
 * third byte on. A reader that expects a block number there loses the path's first two characters and
 * answers for a file that does not exist.
 *
 * The opcode's place is worth naming, because the two references disagree about it: the XNU port for this
 * same modem family reads it out of byte 1 (umi_modem_tftp.h:45), and the Linux port reads it out of the
 * first two bytes as a big-endian number (hakios.mpss.c:4302). The device's requests have byte 0 zero and
 * the opcode in byte 1, so both readings give the same number here. This accepts either shape and says on
 * the log which one arrived, so the device decides rather than this reading.
 *
 * A read is: the modem asks for a file, the host answers with the options it agreed to (or with the first
 * block of the file when the modem asked for no options), and then the modem acknowledges blocks and the
 * host sends the next window of them. A write is: the modem asks to write, the host acknowledges and
 * discards the data, and nothing is stored. That is deliberate, and it is the reference's own behaviour
 * (umi_modem_mhi.inc:787-796): a write of the modem's carrier configuration would rewrite the radio's own
 * configuration, which is not this port's to do.
 *
 * This module is the protocol half only. It parses the requests, keeps the transfers, and builds the
 * blocks; it does not read a file and it does not send anything itself. The board's adapter does both,
 * and the two are joined by the callbacks, the same way the remote file system's two halves are.
 */
#ifndef FUXI_TFTP_H
#define FUXI_TFTP_H

#include <stdint.h>
#include <string.h>

#include "FuxiModemTftp.h"

/* TFTP's QRTR service, and the instance word it is published under. The instance word holds the version in
   its low byte and the instance in its high one. The reference publishes the service at version 1 and at
   version 3, and the modem looks up the one it wants (hakios.mpss.c:4002-4006, umi_modem_mhi.inc:690-691
   and md_tftp_announce). */
#define FUXI_TFTP_SERVICE 0x1000u
#define FUXI_TFTP_VERSION 1u
#define FUXI_TFTP_VERSION_ALT 3u
#define FUXI_TFTP_INSTANCE 0u

/* The local QRTR ports TFTP is served on. The service's own ports are the locator's and the remote file
   system's plus one, so the three services do not collide, and the transfer ports start above the
   bridge's own client ports. */
#define FUXI_TFTP_PORT 0x4003u
#define FUXI_TFTP_PORT_ALT 0x4004u
#define FUXI_TFTP_TRANSFER_PORT_BASE 0x4010u
#define FUXI_TFTP_TRANSFER_SLOTS 4u

/* The opcodes. */
#define FUXI_TFTP_RRQ 1u
#define FUXI_TFTP_WRQ 2u
#define FUXI_TFTP_DATA 3u
#define FUXI_TFTP_ACK 4u
#define FUXI_TFTP_ERROR 5u
#define FUXI_TFTP_OACK 6u

/* The error codes. The first three are the ones the reference sends (umi_modem_mhi.inc:716-725). */
#define FUXI_TFTP_ENOENT 1u
#define FUXI_TFTP_EACCESS 2u
#define FUXI_TFTP_EBADOP 4u

/* The block size the protocol moves data in, and the bounds on the options the modem may ask for. These
   are the reference's ranges (umi_modem_tftp.h:53-58), and the reference's own defaults.
   The block size is capped lower than the reference allows, because a block has to fit in one QRTR
   packet: the layer's scratch buffer is 8192 bytes and the header takes 32 of them. The reference
   caps it the same way, at the packet size less 64 bytes (umi_modem_mhi.inc:788). */
#define FUXI_TFTP_DEFAULT_BLKSIZE 512u
#define FUXI_TFTP_MAX_BLKSIZE 65464u
#define FUXI_TFTP_MIN_BLKSIZE 8u
#define FUXI_TFTP_MAX_RSIZE (100u << 20)
/* The block size this port will actually use: what fits in a packet with room to spare. The modem asks
   for 7680 (measured on 2026-11-22), which is more than a packet holds, so the answer agrees to less and
   the modem takes what the OACK says. */
/* The largest block a DATA packet can carry. A packet is the QRTR header (32), the block header (4) and the
   block, so the link's intent (FUXI_IPCRTR_INTENT_SIZE) less 36 is the ceiling; 4000 leaves room and is what the
   modem accepts.
 *
 * 7680 was tried: the modem asks for it and tqftpserv answers it unchanged, but this path sends rather than
 * queues, and the adapter refused a 7684-byte block mid-window. At 4000 no block was refused. */
#define FUXI_TFTP_PACKET_BLKSIZE 4000u

/* The window this side agrees to, whatever the modem asks for. TFTP lets the server answer a smaller wsize than
 * the request named, and this transport needs it: a GLINK channel has a fixed pool of peer intents (30 on this
 * one) shared with every other packet the port sends, so a window of ten DATA blocks starves after the second
 * block. Measured: the modem held all 30 of its intents, the port had no room 97 million times, and the transfer
 * never passed block two. The reference echoes the modem's ten (umi_modem_mhi.inc:785) because its MHI rings are
 * 32 KiB buffers with no intent accounting (MD_PACKET, :6). */
#define FUXI_TFTP_WINDOW 1u

/* The prefixes the modem asks its files under, and what each of them maps to in the modem's own volume.
   The reference serves these four (hakios.mpss.c, tftp_file_of:4264-4281) and maps every one of them to
   "image/<the rest>" in the modem's FAT partition. The rest of the path is checked for "..", so a request
   cannot climb out of the directory it names. */
#define FUXI_TFTP_PREFIX_COUNT 4u

/* One transfer: a file the modem asked for, and where the transfer stands. */
struct fuxi_tftp_transfer {
    uint8_t used;         /* the slot is taken */
    uint8_t writer;       /* the modem asked to write */
    uint32_t node;        /* the modem's node */
    uint32_t port;        /* the port the modem asked from */
    uint32_t blksize;     /* the block size in force */
    uint32_t wsize;       /* how many blocks the host sends per acknowledgement */
    uint32_t blocks;      /* how many blocks the file has */
    uint64_t seek;        /* where in the file the transfer starts */
    uint64_t limit;       /* how many bytes are left after the seek */
    uint64_t written;     /* how many bytes a writer has sent */
    uint16_t expected;    /* the next block number a writer's data must carry */
    uint8_t slot;         /* which of the transfer slots this one is */
    void *handle;         /* the file the board's adapter opened */
    char name[256];       /* the name the modem used, for a write */
};

/* The state the protocol half keeps. */
struct fuxi_tftp_state {
    struct fuxi_tftp_transfer transfer[FUXI_TFTP_TRANSFER_SLOTS];
    /* The block a read is sent out of: the header and the payload, one of each.
     *
     * They live here and not on the kernel's stack. The answer to an acknowledgement is two buffers of
     * the negotiated block size (4016 and 4000 bytes), and this port's kernel stack is 16 KiB; with the
     * serving function's own answer buffer the three frames came to 12 KiB and the first acknowledgement
     * the modem sent after the port was fixed overflowed it. The link reads packets on one thread, so one
     * buffer in the state is safe; the transfer's own slot is not needed because only one block is built at
     * a time. */
    uint8_t block[FUXI_TFTP_PACKET_BLKSIZE + 16u];
    uint8_t payload[FUXI_TFTP_PACKET_BLKSIZE];
    /* The counters the board's report reads. */
    uint32_t requests;     /* the requests that arrived */
    uint32_t answered;     /* the answers that went out */
    uint32_t reads;        /* the reads that were served */
    uint32_t writes;       /* the writes that were acknowledged and discarded */
    uint32_t refused;      /* the requests that were refused */
    uint32_t missing;      /* the requests for a file that is not there */
    uint32_t blocks_sent;  /* the data blocks that went out */
    uint32_t sends_failed; /* the blocks the adapter refused to send: the window stops there */
};

/* Reads one NUL-terminated string out of a packet: the string's bytes and its length. Returns 0 when the
   string is not terminated inside the packet. */
static inline int fuxi_tftp_string(const uint8_t *p, uint32_t n, uint32_t at, const char **out, uint32_t *length)
{
    if (at >= n) return 0;
    const uint32_t start = at;
    while (at < n && p[at]) at++;
    if (at >= n) return 0;   /* the string runs off the end of the packet */
    *out = (const char *)(p + start);
    *length = at - start;
    return 1;
}

/* A request as it was parsed. */
struct fuxi_tftp_request {
    uint8_t opcode;
    uint8_t wide;          /* the opcode came out of byte 0, not byte 1 */
    uint8_t options;       /* the request carried options */
    char path[256];
    uint32_t path_length;
    uint32_t blksize;
    uint32_t timeoutms;
    uint32_t wsize;
    uint64_t rsize;
    uint64_t seek;
    uint8_t have_tsize;
};

/* Parses a request: the opcode, the path, and the options.
 *
 * The options are the pairs of names and values the modem asked for, and only the ones this protocol
 * knows are kept: blksize, timeoutms, tsize, rsize, wsize and seek. The reference ignores the ones it
 * does not know, and so does this (umi_modem_tftp.h:53-58).
 *
 * The opcode is read out of byte 1, which is where the XNU port for this modem family reads it. A
 * request whose byte 1 is zero and whose byte 0 carries the opcode is accepted too, because the Linux
 * port reads it that way and the two references disagree; the shape that arrived is written to
 * *wide so the caller can say which one it was.
 *
 * Returns 1 when a request was parsed. */
static inline int fuxi_tftp_parse(const uint8_t *p, uint32_t n, struct fuxi_tftp_request *r)
{
    if (!p || !r || n < 4u) return 0;
    memset(r, 0, sizeof *r);
    r->blksize = FUXI_TFTP_DEFAULT_BLKSIZE;
    r->timeoutms = 1000;
    r->wsize = 1;
    /* The opcode is in byte 1. The other shape has it in byte 0, and byte 1 is then zero. */
    if (p[1] >= FUXI_TFTP_RRQ && p[1] <= FUXI_TFTP_OACK) {
        r->opcode = p[1];
    } else if (p[0] >= FUXI_TFTP_RRQ && p[0] <= FUXI_TFTP_OACK && p[1] == 0u) {
        r->opcode = p[0];
        r->wide = 1;
    } else {
        return 0;
    }
    /* A request carries no block number: the path starts at byte 2. A block number would be two bytes of
       the path, and the modem's own requests begin "00 01 2f 72 65 61 64", which is the opcode and then
       "/read". */
    uint32_t at = 2u;
    const char *path = 0;
    uint32_t path_length = 0;
    if (!fuxi_tftp_string(p, n, at, &path, &path_length)) return 0;
    if (!path_length || path_length >= sizeof r->path) return 0;
    memcpy(r->path, path, path_length);
    r->path[path_length] = 0;
    r->path_length = path_length;
    at += path_length + 1u;
    /* The mode string. The reference takes only "octet", in any case. */
    const char *mode = 0;
    uint32_t mode_length = 0;
    if (!fuxi_tftp_string(p, n, at, &mode, &mode_length)) return 0;
    at += mode_length + 1u;
    if (mode_length != 5u) return 0;
    for (uint32_t i = 0; i < 5u; ++i) {
        if ((mode[i] | 0x20) != "octet"[i]) return 0;
    }
    /* The options, in pairs. */
    while (at < n) {
        const char *name = 0;
        uint32_t name_length = 0;
        if (!fuxi_tftp_string(p, n, at, &name, &name_length)) return 0;
        at += name_length + 1u;
        const char *value = 0;
        uint32_t value_length = 0;
        if (!fuxi_tftp_string(p, n, at, &value, &value_length)) return 0;
        at += value_length + 1u;
        r->options = 1;
        /* The value is a number. A value that is not one is ignored, the way the reference ignores an
           option it does not know. */
        uint64_t number = 0;
        int numeric = value_length > 0u;
        for (uint32_t i = 0; i < value_length; ++i) {
            if (value[i] < '0' || value[i] > '9') { numeric = 0; break; }
            number = number * 10u + (uint64_t)(value[i] - '0');
            if (number > 0xffffffffull) { numeric = 0; break; }
        }
        if (!numeric) continue;
        if (name_length == 7u && !memcmp(name, "blksize", 7u)) {
            if (number >= FUXI_TFTP_MIN_BLKSIZE && number <= FUXI_TFTP_MAX_BLKSIZE) {
                r->blksize = number > FUXI_TFTP_PACKET_BLKSIZE ? FUXI_TFTP_PACKET_BLKSIZE : (uint32_t)number;
            }
        } else if (name_length == 9u && !memcmp(name, "timeoutms", 9u)) {
            if (number >= 1u && number <= 255000u) r->timeoutms = (uint32_t)number;
        } else if (name_length == 5u && !memcmp(name, "tsize", 5u)) {
            r->have_tsize = 1;
        } else if (name_length == 5u && !memcmp(name, "rsize", 5u)) {
            if (number >= 1u && number <= FUXI_TFTP_MAX_RSIZE) r->rsize = number;
        } else if (name_length == 5u && !memcmp(name, "wsize", 5u)) {
            if (number >= 1u && number <= 65535u) r->wsize = (uint32_t)number;
        } else if (name_length == 4u && !memcmp(name, "seek", 4u)) {
            r->seek = number;
        }
    }
    return 1;
}

/* Writes a number as the decimal digits a TFTP option carries. Returns 0 when it does not fit. */
static inline int fuxi_tftp_put_number(uint8_t *out, uint32_t cap, uint32_t *at, uint64_t value)
{
    char digits[24];
    uint32_t n = 0;
    do {
        digits[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value);
    if (*at + n + 1u > cap) return 0;
    while (n) out[(*at)++] = (uint8_t)digits[--n];
    out[(*at)++] = 0;
    return 1;
}

/* Writes one option: the name, then the value. */
static inline int fuxi_tftp_put_option(uint8_t *out, uint32_t cap, uint32_t *at, const char *name, uint64_t value)
{
    const uint32_t n = (uint32_t)strlen(name);
    if (*at + n + 1u > cap) return 0;
    memcpy(out + *at, name, n);
    *at += n;
    out[(*at)++] = 0;
    return fuxi_tftp_put_number(out, cap, at, value);
}

/* Builds the answer that agrees to the options: the OACK.
 *
 * The options go out in the reference's order (umi_modem_tftp.h:73-83): blksize, timeoutms, tsize, wsize,
 * rsize, seek. The size is the file's own, and it is only sent when the modem asked for it. */
static inline uint32_t fuxi_tftp_build_oack(uint8_t *out, uint32_t cap, const struct fuxi_tftp_request *r,
                                            uint64_t file_size)
{
    uint32_t at = 2u;
    out[0] = 0u;
    out[1] = (uint8_t)FUXI_TFTP_OACK;
    if (!fuxi_tftp_put_option(out, cap, &at, "blksize", r->blksize)) return 0;
    if (!fuxi_tftp_put_option(out, cap, &at, "timeoutms", r->timeoutms)) return 0;
    if (r->have_tsize && !fuxi_tftp_put_option(out, cap, &at, "tsize", file_size)) return 0;
    if (!fuxi_tftp_put_option(out, cap, &at, "wsize", r->wsize)) return 0;
    if (r->rsize && !fuxi_tftp_put_option(out, cap, &at, "rsize", r->rsize)) return 0;
    if (r->seek && !fuxi_tftp_put_option(out, cap, &at, "seek", r->seek)) return 0;
    return at;
}

/* Builds one block of a file: the opcode, the block number, and the bytes.
 *
 * A block shorter than the block size ends the transfer, and that is how the last block is sent (the
 * reference's umi_tftp_block_length: the length is what is left of the file, and it is zero once the
 * file is over). */
static inline uint32_t fuxi_tftp_build_data(uint8_t *out, uint32_t cap, uint16_t block, const uint8_t *data,
                                            uint32_t length)
{
    if (cap < 4u + length) return 0;
    out[0] = 0u;
    out[1] = (uint8_t)FUXI_TFTP_DATA;
    out[2] = (uint8_t)(block >> 8);
    out[3] = (uint8_t)(block & 0xffu);
    if (length) memcpy(out + 4, data, length);
    return 4u + length;
}

/* Builds an acknowledgement: the opcode and the block number. */
static inline uint32_t fuxi_tftp_build_ack(uint8_t *out, uint32_t cap, uint16_t block)
{
    if (cap < 4u) return 0;
    out[0] = 0u;
    out[1] = (uint8_t)FUXI_TFTP_ACK;
    out[2] = (uint8_t)(block >> 8);
    out[3] = (uint8_t)(block & 0xffu);
    return 4u;
}

/* Builds an error: the opcode, the code, and the text.
 *
 * The code is the two bytes the block number would be, which is where the Linux reference puts it
 * (hakios.mpss.c:4102-4115). The XNU reference puts a single byte there and never needed a code above
 * 255, so the two agree on every code this uses. */
static inline uint32_t fuxi_tftp_build_error(uint8_t *out, uint32_t cap, uint16_t code, const char *text)
{
    const uint32_t length = text ? (uint32_t)strlen(text) + 1u : 0u;
    if (cap < 4u + length) return 0;
    out[0] = 0u;
    out[1] = (uint8_t)FUXI_TFTP_ERROR;
    out[2] = (uint8_t)(code >> 8);
    out[3] = (uint8_t)(code & 0xffu);
    if (length) memcpy(out + 4, text, length);
    return 4u + length;
}

/* How many blocks a transfer of limit bytes has: one more than the full blocks, and the last one is
   short (the reference's umi_tftp_blocks). */
static inline uint32_t fuxi_tftp_blocks(uint64_t limit, uint32_t blksize)
{
    if (!blksize) return 0;
    return (uint32_t)(limit / blksize + 1u);
}

/* How long one block is: what is left of the file from the block's own offset, and zero once the file is
   over. */
static inline uint32_t fuxi_tftp_block_length(uint64_t limit, uint32_t blksize, uint32_t block)
{
    const uint64_t offset = (uint64_t)(block - 1u) * blksize;
    if (offset >= limit) return 0;
    return (uint32_t)(limit - offset < blksize ? limit - offset : blksize);
}

/* Sends one block of the file, and says whether the transfer is over.
 *
 * The block is read out of the file at the offset its own number gives, and it is sent from the
 * transfer's own port, which is what tells the modem which transfer it belongs to.
 *
 * The last block is the short one, and a short block ends the transfer: that is how TFTP marks the end
 * of a file, and it is what the reference relies on (umi_modem_mhi.inc:838-840 and the block length
 * helper next to it). */
static inline int fuxi_tftp_send_block(struct fuxi_tftp_state *state, struct fuxi_tftp_transfer *t,
                                       const struct fuxi_tftp_ops *ops, void *context, uint32_t block)
{
    uint8_t *out = state->block;
    uint8_t *data = state->payload;
    const uint32_t n = fuxi_tftp_block_length(t->limit, t->blksize, block);
    if (n && (!ops->read || ops->read(context, t->handle, t->seek + (uint64_t)(block - 1u) * t->blksize, data, n)))
        return -1;
    const uint32_t length = fuxi_tftp_build_data(out, sizeof state->block, (uint16_t)block, data, n);
    if (!length) return -1;
    if (!ops->send || ops->send(context, FUXI_TFTP_TRANSFER_PORT_BASE + t->slot, t->node, t->port, out, length)) return -1;
    state->blocks_sent++;
    return n < t->blksize ? 1 : 0;   /* the transfer is over when the block was short */
}

/* Answers an acknowledgement: the modem has the blocks up to the one it names, and the next window of
 * blocks goes out.
 *
 * The window is as wide as the modem asked for (its wsize option), and the reference's rule is the
 * same: an ACK of n sends the blocks n+1 to n+wsize (umi_modem_mhi.inc:838-840). The last block of the
 * file is the short one, so the window ends there.
 *
 * The transfer is over once the modem has acknowledged the last block, and the slot it held is freed
 * then: the reference frees it on exactly that condition (umi_modem_mhi.inc:838, an acknowledgement of
 * the last block closes the transfer). The slot is not freed when the last block goes out, because the
 * modem still has to acknowledge it and the slot must still be there to answer it.
 *
 * Returns 1 when the window went out. */
static inline int fuxi_tftp_answer_ack(struct fuxi_tftp_state *state, struct fuxi_tftp_transfer *t,
                                       const struct fuxi_tftp_ops *ops, void *context, uint16_t acked)
{
    if ((uint32_t)acked >= t->blocks) {
        /* The modem has the whole file: the transfer is over and the slot is free. */
        if (t->handle && ops->close) ops->close(context, t->handle);
        memset(t, 0, sizeof *t);
        return 1;
    }
    for (uint32_t block = (uint32_t)acked + 1u; block <= (uint32_t)acked + t->wsize; ++block) {
        if (block > t->blocks) break;
        const int over = fuxi_tftp_send_block(state, t, ops, context, block);
        if (over < 0) {
            /* The adapter could not take this block right now. The window stops, and the transfer stays: the next
               acknowledgement from the modem resumes it, which is what tqftpserv does when its send fails
               (tqftpserv.c:801-804 breaks the loop and the session is kept; only a failed receive closes it,
               :1006-1008). Telling the caller the window failed instead would drop every block the modem had
               not yet acknowledged and lose the transfer. */
            state->sends_failed++;
            return 1;
        }
        if (over) break;   /* the short block ended the transfer */
    }
    return 1;
}

/* Serves one packet that arrived for TFTP.
 *
 * The packet is either a request on the service's own port, or a block or an acknowledgement on one of
 * the transfer ports. A request is answered here; a block or an acknowledgement is answered by sending
 * the next window of the file.
 *
 * The port the request came in on says which of the two it is: the service's own ports take the
 * requests, and the transfer ports take the blocks and the acknowledgements (the reference's own
 * dispatch, umi_modem_mhi.inc:906-909).
 *
 * The shape of the request is written to *opcode_out and *wide_out, so the caller can put the bytes on
 * the log.
 *
 * Returns 1 when the packet was served, and 0 when it was not one this answers. */
static inline int fuxi_tftp_serve(struct fuxi_tftp_state *state, uint32_t dst_port, uint32_t src_node, uint32_t src_port,
                                  const uint8_t *body, uint32_t length, const struct fuxi_tftp_ops *ops, void *context,
                                  uint32_t *opcode_out, uint32_t *wide_out)
{
    if (!state || !body || !ops) return 0;

    /* A block or an acknowledgement on one of the transfer ports. */
    if (dst_port >= FUXI_TFTP_TRANSFER_PORT_BASE &&
        dst_port < FUXI_TFTP_TRANSFER_PORT_BASE + FUXI_TFTP_TRANSFER_SLOTS) {
        const unsigned slot = dst_port - FUXI_TFTP_TRANSFER_PORT_BASE;
        struct fuxi_tftp_transfer *t = &state->transfer[slot];
        if (!t->used || t->node != src_node) return 0;
        if (length < 4u) return 0;
        const uint8_t opcode = body[1] ? body[1] : body[0];
        const uint16_t block = (uint16_t)((uint16_t)body[2] << 8 | body[3]);
        if (opcode_out) *opcode_out = opcode;
        if (wide_out) *wide_out = body[1] ? 0u : 1u;
        state->requests++;
        if (opcode == FUXI_TFTP_ACK) {
            /* The modem has the blocks up to the one it names; the next window goes out. */
            if (t->writer) return 1;
            if (!fuxi_tftp_answer_ack(state, t, ops, context, block)) return 0;
            state->answered++;
            return 1;
        }
        if (opcode == FUXI_TFTP_DATA) {
            /* A writer's data goes into the adapter's memory, under the name the modem used, so the modem
               can read it back. Nothing is written to the modem's storage: the store is memory, for the
               run, and the reference does the same (hakios.mpss.c:4214-4236). */
            if (t->writer) {
                if (block != t->expected) return 0;
                t->expected++;
                const uint32_t n = length - 4u;
                if (n && ops->store) ops->store(context, t->name, (uint32_t)strlen(t->name), body + 4u, n);
                t->written += n;
                /* The acknowledgement comes every wsize blocks, or on the last one, whichever comes first
                   (hakios.mpss.c:4207-4209 and umi_modem_mhi.inc:838-840). */
                const int last = n < t->blksize;
                if (block % t->wsize == 0 || last) {
                    uint8_t ack[8];
                    const uint32_t a = fuxi_tftp_build_ack(ack, sizeof ack, block);
                    if (!a) return 0;
                    if (!ops->send || ops->send(context, dst_port, t->node, t->port, ack, a)) return 0;
                    state->answered++;
                }
                if (last) {
                    /* The last block: the file is complete, and the transfer is over. */
                    memset(t, 0, sizeof *t);
                }
                return 1;
            }
            return 1;
        }
        if (opcode == FUXI_TFTP_ERROR) {
            /* The modem ended the transfer: the file is released. */
            if (t->handle && ops->close) ops->close(context, t->handle);
            memset(t, 0, sizeof *t);
            return 1;
        }
        return 0;
    }

    /* A request on the service's own port. */
    if (dst_port != FUXI_TFTP_PORT && dst_port != FUXI_TFTP_PORT_ALT) return 0;
    struct fuxi_tftp_request r;
    if (!fuxi_tftp_parse(body, length, &r)) {
        state->refused++;
        return 0;
    }
    if (opcode_out) *opcode_out = r.opcode;
    if (wide_out) *wide_out = r.wide;
    state->requests++;

    /* Only the options answer, an acknowledgement and an error are built here; the blocks themselves are
       built in the state's buffers. 512 bytes covers every one of those with room to spare. */
    uint8_t out[512];

    if (r.opcode == FUXI_TFTP_WRQ) {
        /* A write goes into the adapter's memory, under the name the modem used, and the modem can read
           it back. Nothing is written to the modem's storage: the carrier configuration is the radio's
           own, and rewriting it is not this port's to do. The reference keeps the written file in memory
           for the run as well (hakios.mpss.c:4214-4236). */
        state->writes++;
        unsigned slot = 0;
        while (slot < FUXI_TFTP_TRANSFER_SLOTS && state->transfer[slot].used) slot++;
        if (slot == FUXI_TFTP_TRANSFER_SLOTS) {
            state->refused++;
            return 0;
        }
        struct fuxi_tftp_transfer *w = &state->transfer[slot];
        memset(w, 0, sizeof *w);
        w->used = 1;
        w->writer = 1;
        w->node = src_node;
        w->port = src_port;
        w->slot = (uint8_t)slot;
        w->expected = 1;
        w->blksize = r.blksize;
        w->wsize = r.wsize;
        /* The name the modem used is kept, so the blocks that follow go under it and a read of the same
           name finds them. */
        if (r.path_length < sizeof w->name) {
            memcpy(w->name, r.path, r.path_length);
            w->name[r.path_length] = 0;
        }
        uint32_t n;
        if (r.options) {
            n = fuxi_tftp_build_oack(out, sizeof out, &r, 0);
        } else {
            n = fuxi_tftp_build_ack(out, sizeof out, 0);
        }
        if (!n) return 0;
        if (!ops->send || ops->send(context, FUXI_TFTP_TRANSFER_PORT_BASE + slot, src_node, src_port, out, n)) return 0;
        state->answered++;
        return 1;
    }

    if (r.opcode != FUXI_TFTP_RRQ) {
        state->refused++;
        return 0;
    }

    /* The file the modem asks for. */
    void *handle = 0;
    if (!ops->open || ops->open(context, r.path, r.path_length, &handle)) {
        state->missing++;
        const uint32_t n = fuxi_tftp_build_error(out, sizeof out, FUXI_TFTP_ENOENT, "file not found");
        if (!n) return 0;
        if (!ops->send || ops->send(context, dst_port, src_node, src_port, out, n)) return 0;
        state->answered++;
        return 1;
    }

    const uint64_t size = ops->size ? ops->size(context, handle) : 0;

    unsigned slot = 0;
    while (slot < FUXI_TFTP_TRANSFER_SLOTS && state->transfer[slot].used) slot++;
    if (slot == FUXI_TFTP_TRANSFER_SLOTS) {
        if (ops->close) ops->close(context, handle);
        const uint32_t n = fuxi_tftp_build_error(out, sizeof out, FUXI_TFTP_EACCESS, "busy");
        if (!n) return 0;
        if (!ops->send || ops->send(context, dst_port, src_node, src_port, out, n)) return 0;
        state->answered++;
        return 1;
    }
    struct fuxi_tftp_transfer *t = &state->transfer[slot];
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->node = src_node;
    t->port = src_port;
    t->slot = (uint8_t)slot;
    t->blksize = r.blksize;
    if (r.wsize > FUXI_TFTP_WINDOW) r.wsize = FUXI_TFTP_WINDOW;
    t->wsize = r.wsize;
    t->seek = r.seek;
    t->limit = size > r.seek ? size - r.seek : 0;
    if (r.rsize && r.rsize < t->limit) t->limit = r.rsize;
    t->blocks = fuxi_tftp_blocks(t->limit, t->blksize);
    t->handle = handle;

    if (r.options) {
        /* The modem asked for options, so the answer agrees to them and the file itself follows.
           The answer goes out from the transfer's own port, which is what tells the modem which transfer
           it belongs to: the modem sends its acknowledgements and its errors back to the port the
           answer came from, and a service port has no transfer waiting on it. The reference sends the
           read's answer from the transfer port too (umi_modem_mhi.inc:784, the same src for the write
           and for the read). */
        const uint32_t n = fuxi_tftp_build_oack(out, sizeof out, &r, size);
        if (!n) return 0;
        if (!ops->send || ops->send(context, FUXI_TFTP_TRANSFER_PORT_BASE + slot, src_node, src_port, out, n)) return 0;
    } else {
        /* No options: the first block of the file goes straight out. */
        if (fuxi_tftp_send_block(state, t, ops, context, 1u) < 0) return 0;
    }
    state->reads++;
    state->answered++;
    return 1;
}

#endif
