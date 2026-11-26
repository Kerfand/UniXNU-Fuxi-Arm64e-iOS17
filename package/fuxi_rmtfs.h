/* fuxi 2026-11-26: from ljhhkx529/QcomBaseband (fuxi_rmtfs.h), the modem GLINK/QRTR renamed modem_glink_/modem_qrtr_. */
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * RMTFS: the remote file system the modem reads its own storage through.
 *
 * The modem keeps four files on the phone's storage, which it cannot reach itself: the two modem state
 * partitions and the two calibration ones. It asks the application processor for them over QRTR, on service
 * 14, with five messages:
 *
 *   OPEN 1          open one file by its path, and get back the number the other messages use
 *   CLOSE 2         close it
 *   RW_IOVEC 3      read or write sectors, through a buffer both sides can reach
 *   ALLOC_BUFF 4    hand the modem the address of that buffer
 *   GET_DEV_ERROR 5 ask whether the device behind a file has an error
 *
 * The buffer is the whole point. The modem cannot read the partitions itself, so it asks the application
 * processor to move the sectors for it: RW_IOVEC names, for each run of sectors, where in the file they
 * come from and where in the buffer they go. The buffer is the memory the modem was given by the secure
 * world, so the modem can write into it directly and the application processor sees the bytes.
 *
 * This module is the protocol half only: it parses the five requests, answers them, and keeps the file
 * numbers. It does not read a partition and it does not touch the buffer; the board's adapter does that,
 * and the two are joined by the callbacks. The paths the modem asks for, the partition behind each one,
 * the buffer's address and the adapter's contract are the board's own facts, and they are in the board's
 * header, which this one includes.
 *
 * The wire format is QMI's: a type byte, a transaction, the message id, the length of the TLVs, and then
 * the TLVs. A TLV is a type byte, a two-byte length and the value. Two shapes are worth naming:
 *
 *   - the path in OPEN is the TLV's value itself, with no length of its own in front of it, so the TLV's
 *     own length is the path's length;
 *   - the iovec's entries in RW_IOVEC are the TLV's value with a count byte in front of them, and each
 *     entry is three 32-bit words: the sector in the file, the physical address in the buffer, and the
 *     number of sectors.
 *
 * Every answer carries the request's transaction and message id back, so the modem can match it, and the
 * result comes first: the answer is a seven-byte header, the result TLV, and then whatever the message
 * carries besides it.
 *
 * The reference port for the same modem in XNU is a hardware-proven implementation of this protocol
 * (hakios.mpss.c, rmtfs_receive). The wire format here is that one, read from it.
 */
#ifndef FUXI_RMTFS_H
#define FUXI_RMTFS_H

#include <stdint.h>
#include <string.h>

#include "FuxiModemRmtfs.h"

/* RMTFS's QRTR service, and the instance word it is published under: version 1, instance 0, which is the
   word 0x0001. The reference publishes it the same way (hakios.mpss.c, rmtfs_start: RMTFS_VERSION |
   RMTFS_INSTANCE << 8). */
#define FUXI_RMTFS_SERVICE 0x0eu
#define FUXI_RMTFS_VERSION 1u
#define FUXI_RMTFS_INSTANCE 0u

/* The local QRTR port RMTFS is served on. It is the locator's port plus one, so the two services do not
   collide and the bridge's own client ports stay clear of both. */
#define FUXI_RMTFS_PORT 0x4002u

/* The five message ids. */
#define FUXI_RMTFS_OPEN 1u
#define FUXI_RMTFS_CLOSE 2u
#define FUXI_RMTFS_RW_IOVEC 3u
#define FUXI_RMTFS_ALLOC_BUFF 4u
#define FUXI_RMTFS_GET_DEV_ERROR 5u

/* The result TLV's two halves, and the errors. A message this does not know is answered with a failure and
   the malformed-message error, and with nothing else. */
#define FUXI_RMTFS_RESULT_SUCCESS 0u
#define FUXI_RMTFS_RESULT_FAILURE 1u
#define FUXI_RMTFS_ERR_NONE 0u
#define FUXI_RMTFS_ERR_INTERNAL 1u
#define FUXI_RMTFS_ERR_MALFORMED 2u

/* The module's own state: which files the modem has open. The reference keeps the same thing
   (hakios.mpss.c: rmtfs_files[f].open), and a sector may only be moved for a file that is open. */
struct fuxi_rmtfs_state {
    uint8_t open[FUXI_RMTFS_FILES];
};

/* The value of the first TLV of the given type in a QMI message, or null when the message has none. The
   walk stops at the message's own length, so a truncated TLV is not read past the end. */
static const uint8_t *fuxi_rmtfs_tlv(const uint8_t *m, uint32_t length, uint8_t type, uint32_t *size)
{
    if (!m || length < 7u) return 0;
    for (uint32_t at = 7u; at + 3u <= length;) {
        const uint32_t n = (uint32_t)m[at + 1] | (uint32_t)m[at + 2] << 8;
        if (at + 3u + n > length) return 0;
        if (m[at] == type) {
            if (size) *size = n;
            return m + at + 3u;
        }
        at += 3u + n;
    }
    return 0;
}

/* Appends one TLV to the answer being built. */
static int fuxi_rmtfs_tlv_put(uint8_t *out, uint32_t cap, uint32_t *at, uint8_t type, const void *value, uint32_t size)
{
    if (!out || !at || size > 0xffffu || *at > cap || cap - *at < 3u + size) return -1;
    out[(*at)++] = type;
    out[(*at)++] = (uint8_t)size;
    out[(*at)++] = (uint8_t)(size >> 8);
    memcpy(out + *at, value, size);
    *at += size;
    return 0;
}

/* Reads a 32-bit value out of a TLV's value, little endian. */
static uint32_t fuxi_rmtfs_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The result TLV, which comes first in every answer: the type byte 0x02, the length 4, and then the result
   and the error, each a 16-bit little-endian value. The reference writes it in the same place
   (hakios.mpss.c: answer[7] = 2, answer[8] = 4, answer[10] = result, answer[12] = error), and it writes it
   before the answer's other TLV, which then follow it. */
static void fuxi_rmtfs_result(uint8_t *out, uint32_t cap, uint32_t *at, uint32_t result, uint32_t error)
{
    if (!out || !at || cap < 14u || *at != 7u) return;
    out[7] = 0x02u;
    out[8] = 4u;
    out[9] = 0u;
    out[10] = (uint8_t)result;
    out[11] = (uint8_t)(result >> 8);
    out[12] = (uint8_t)error;
    out[13] = (uint8_t)(error >> 8);
    *at = 14u;
}

/* Finishes an answer: the length of the TLVs goes into the header's own length field. The length is the
   result TLV's seven bytes plus whatever followed it. */
static void fuxi_rmtfs_end(uint8_t *out, uint32_t at, uint32_t *length)
{
    out[5] = (uint8_t)((at - 7u) & 0xffu);
    out[6] = (uint8_t)((at - 7u) >> 8);
    if (length) *length = at;
}

/* The file a path names, or zero when the path is not one of the four. The number the modem uses is this
   one's place in the table plus one. */
static uint32_t fuxi_rmtfs_number_for(const uint8_t *path, uint32_t path_length)
{
    if (!path) return 0;
    for (uint32_t i = 0; i < FUXI_RMTFS_FILES; ++i) {
        const uint32_t length = (uint32_t)strlen(fuxi_rmtfs_files[i].path);
        if (length == path_length && !memcmp(path, fuxi_rmtfs_files[i].path, length)) return i + 1u;
    }
    return 0;
}

/* Serves one request that arrived at RMTFS's port.
 *
 * The answer is a QMI response that carries the request's transaction and message id back. What each of
 * the five messages answers is:
 *
 *   OPEN          the path is TLV 1, a plain string. The answer carries the result and the file's number,
 *                 in TLV 0x10, as a 32-bit value.
 *   CLOSE         the file's number is TLV 1. The answer carries the result and nothing else.
 *   RW_IOVEC      the file's number is TLV 1, the direction is TLV 2, and the entries are TLV 3: a byte of
 *                 count and then the entries, each three 32-bit words. The answer carries the result.
 *   ALLOC_BUFF    the size is TLV 2. The answer carries the result and the buffer's address, in TLV 0x10,
 *                 as a 64-bit value.
 *   GET_DEV_ERROR the answer carries the result and the status, in TLV 0x10, a single byte.
 *
 * Any other message id is answered with a failure and the malformed-message error, and with nothing else,
 * so a message this does not know is never answered with a success.
 *
 * Returns 1 when an answer was built, 0 when there is nothing to answer: a packet that is not a QMI
 * request, or an answer that does not fit. */
static int fuxi_rmtfs_serve(struct fuxi_rmtfs_state *state, const uint8_t *req, uint32_t req_length, uint8_t *out,
                             uint32_t cap, uint32_t *length, const struct fuxi_rmtfs_ops *ops, void *context,
                             uint32_t *message_out)
{
    if (!state || !req || !out || !length || !ops || req_length < 7u || cap < 14u) return 0;
    /* A response, not a request: there is nothing to answer. */
    if (req[0] != 0x00u) return 0;
    const uint32_t message = (uint32_t)req[3] | (uint32_t)req[4] << 8;
    if (message_out) *message_out = message;

    /* The answer's header: the request's own bytes, with the type turned into a response. The transaction
       and the message id are echoed, so the modem can match the answer to its request. */
    memcpy(out, req, 7u);
    out[0] = 0x02u;

    /* The result TLV comes first in every answer, and the answer's own TLVs follow it. The reference builds
       the answer the same way round: the result is written into bytes 7 to 13 and the answer's other TLV
       starts at byte 14 (hakios.mpss.c, rmtfs_receive). */
    uint32_t at = 7u;

    if (message == FUXI_RMTFS_OPEN) {
        /* The path is TLV 1. It is the TLV's value itself: no length of its own in front of it, so the
           TLV's own length is the path's length. */
        uint32_t path_length = 0;
        const uint8_t *path = fuxi_rmtfs_tlv(req, req_length, 1u, &path_length);
        const uint32_t number = fuxi_rmtfs_number_for(path, path_length);
        if (!number || !ops->open || ops->open(context, path, path_length)) {
            fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_FAILURE, FUXI_RMTFS_ERR_INTERNAL);
            fuxi_rmtfs_end(out, at, length);
            return 1;
        }
        state->open[number - 1u] = 1;
        fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_SUCCESS, FUXI_RMTFS_ERR_NONE);
        const uint8_t number_bytes[4] = {(uint8_t)number, (uint8_t)(number >> 8), (uint8_t)(number >> 16),
                                        (uint8_t)(number >> 24)};
        if (fuxi_rmtfs_tlv_put(out, cap, &at, 0x10u, number_bytes, sizeof number_bytes)) return 0;
        fuxi_rmtfs_end(out, at, length);
        return 1;
    }

    if (message == FUXI_RMTFS_CLOSE) {
        uint32_t size = 0;
        const uint8_t *value = fuxi_rmtfs_tlv(req, req_length, 1u, &size);
        const uint32_t number = value && size == 4u ? fuxi_rmtfs_le32(value) : 0u;
        if (number && number <= FUXI_RMTFS_FILES) {
            state->open[number - 1u] = 0;
            if (ops->close) ops->close(context, number);
        }
        fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_SUCCESS, FUXI_RMTFS_ERR_NONE);
        fuxi_rmtfs_end(out, at, length);
        return 1;
    }

    if (message == FUXI_RMTFS_RW_IOVEC) {
        uint32_t size = 0;
        const uint8_t *value = fuxi_rmtfs_tlv(req, req_length, 1u, &size);
        const uint32_t number = value && size == 4u ? fuxi_rmtfs_le32(value) : 0u;
        uint32_t direction_size = 0;
        const uint8_t *direction = fuxi_rmtfs_tlv(req, req_length, 2u, &direction_size);
        uint32_t entries_size = 0;
        const uint8_t *entries = fuxi_rmtfs_tlv(req, req_length, 3u, &entries_size);
        /* The entries' TLV must be exactly a count byte and that many entries: anything else is malformed,
           and so is a file that is not open (hakios.mpss.c checks the same two). */
        if (!number || number > FUXI_RMTFS_FILES || !state->open[number - 1u] || !direction || direction_size != 1u ||
            !entries || entries_size < 1u || entries_size != 1u + 12u * entries[0]) {
            fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_FAILURE, FUXI_RMTFS_ERR_MALFORMED);
            fuxi_rmtfs_end(out, at, length);
            return 1;
        }
        /* The direction: anything but zero is a write. The reference reads it the same way
           (hakios.mpss.c: `write = direction[0] != 0`). */
        const uint8_t is_write = direction[0] != 0;
        const uint32_t count = entries[0];
        uint32_t buffer_size = 0;
        const uint64_t buffer = ops->buffer ? ops->buffer(context, &buffer_size) : 0;
        const uint64_t file_size = ops->file_size ? ops->file_size(context, number) : 0;
        if (!buffer || !buffer_size) {
            fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_FAILURE, FUXI_RMTFS_ERR_INTERNAL);
            fuxi_rmtfs_end(out, at, length);
            return 1;
        }
        for (uint32_t e = 0; e < count; ++e) {
            const uint8_t *entry = entries + 1u + 12u * e;
            const uint32_t sector = fuxi_rmtfs_le32(entry);
            const uint32_t physical = fuxi_rmtfs_le32(entry + 4);
            const uint32_t sectors = fuxi_rmtfs_le32(entry + 8);
            const uint64_t bytes = (uint64_t)sectors * FUXI_RMTFS_SECTOR_SIZE;
            /* The buffer's address is absolute: the reference refuses anything below the buffer or past
               its end (hakios.mpss.c: `physical < rmtfs_buffer_pa || physical - rmtfs_buffer_pa + bytes
               > RMTFS_BUFFER_SIZE`). The file's own size bounds the sectors too (hakios.mpss.c:
               `sector * RMTFS_SECTOR + bytes > rmtfs_files[file].size`). */
            if ((uint64_t)physical < buffer || (uint64_t)physical - buffer + bytes > (uint64_t)buffer_size ||
                (uint64_t)sector * FUXI_RMTFS_SECTOR_SIZE + bytes > file_size) {
                fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_FAILURE, FUXI_RMTFS_ERR_MALFORMED);
                fuxi_rmtfs_end(out, at, length);
                return 1;
            }
            for (uint32_t s = 0; s < sectors; ++s) {
                const uint32_t at_sector = sector + s;
                const uint32_t at_buffer = physical + s * FUXI_RMTFS_SECTOR_SIZE;
                const int moved = is_write ? (ops->write ? ops->write(context, number, at_sector, at_buffer) : -1)
                                          : (ops->read ? ops->read(context, number, at_sector, at_buffer) : -1);
                if (moved) {
                    fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_FAILURE, FUXI_RMTFS_ERR_INTERNAL);
                    fuxi_rmtfs_end(out, at, length);
                    return 1;
                }
            }
        }
        fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_SUCCESS, FUXI_RMTFS_ERR_NONE);
        fuxi_rmtfs_end(out, at, length);
        return 1;
    }

    if (message == FUXI_RMTFS_ALLOC_BUFF) {
        uint32_t size = 0;
        const uint8_t *value = fuxi_rmtfs_tlv(req, req_length, 2u, &size);
        uint32_t buffer_size = 0;
        const uint64_t buffer = ops->buffer ? ops->buffer(context, &buffer_size) : 0;
        /* The size the modem asks for is checked against the buffer: the buffer is a fixed window, and a
           request for more than it holds cannot be answered with a buffer. */
        if (!value || size != 4u || !buffer || !buffer_size || fuxi_rmtfs_le32(value) > buffer_size) {
            fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_FAILURE, FUXI_RMTFS_ERR_INTERNAL);
            fuxi_rmtfs_end(out, at, length);
            return 1;
        }
        fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_SUCCESS, FUXI_RMTFS_ERR_NONE);
        uint8_t address[8];
        for (unsigned b = 0; b < 8u; ++b) address[b] = (uint8_t)(buffer >> (8u * b));
        if (fuxi_rmtfs_tlv_put(out, cap, &at, 0x10u, address, sizeof address)) return 0;
        fuxi_rmtfs_end(out, at, length);
        return 1;
    }

    if (message == FUXI_RMTFS_GET_DEV_ERROR) {
        /* The status: zero, which is no error. The device behind a file is the partition, and a partition
           that was read into memory whole has no error. */
        fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_SUCCESS, FUXI_RMTFS_ERR_NONE);
        const uint8_t status = 0;
        if (fuxi_rmtfs_tlv_put(out, cap, &at, 0x10u, &status, 1u)) return 0;
        fuxi_rmtfs_end(out, at, length);
        return 1;
    }

    /* A message this does not know: a failure and the malformed-message error, and nothing else. */
    fuxi_rmtfs_result(out, cap, &at, FUXI_RMTFS_RESULT_FAILURE, FUXI_RMTFS_ERR_MALFORMED);
    fuxi_rmtfs_end(out, at, length);
    return 1;
}

#endif
