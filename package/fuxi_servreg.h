/* fuxi 2026-11-26: from ljhhkx529/QcomBaseband (fuxi_servreg.h), the modem GLINK/QRTR renamed modem_glink_/modem_qrtr_. */
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The service registry's locator: the host service the modem asks where its own services are.
 *
 * The modem's protection domains register their services with a registry whose locator lives on the application
 * processor (QRTR service 64). Asked for a service by name, the locator answers with the domains that provide
 * it and their QMI instance.
 *
 * The table is the modem's own, read off the device's firmware partition: /image/modemr.jsn names one
 * protection domain, msm/modem/root_pd with qmi_instance_id 180, and inside it two services, tms/servreg and
 * gps/gps_service, both without service data. The board's probe dumps that file on every boot of the mode
 * that runs this, so a run's log holds the table the answer was built from.
 *
 * The answer's shape is the one Linux's protection-domain mapper answers with (Linux's qcom_pd_mapper, and
 * the reference port's umi3_servreg_domains): a QMI response carrying the result in TLV 0x02, the number of
 * domains in TLV 0x10, the table's revision in TLV 0x11 and the list itself in TLV 0x12.
 */
#ifndef FUXI_SERVREG_H
#define FUXI_SERVREG_H

#include <stdint.h>
#include <string.h>

/* The locator's QRTR service, and the instance word it is published under: version 1, instance 1, which is
   the word 0x0101 (257). The reference port publishes it the same way (its MD_PDM_PORT at instance 257). */
#define FUXI_LOCATOR_SERVICE 0x40u
#define FUXI_LOCATOR_VERSION 1u
#define FUXI_LOCATOR_INSTANCE 1u

/* The local QRTR port the locator serves on. The reference port publishes the locator on this same port
   number, and the bridge's own client ports start above it, so the two do not collide. */
#define FUXI_LOCATOR_PORT 0x4001u

/* GET_DOMAIN_LIST: the only message the locator answers. */
#define FUXI_SERVREG_GET_DOMAIN_LIST 0x0021u

/* The QMI result TLV's two halves: the result, and the error. A message this does not know is answered with
   a failure and the invalid-message-id error, and with nothing else. */
#define FUXI_QMI_RESULT_SUCCESS 0u
#define FUXI_QMI_RESULT_FAILURE 1u
#define FUXI_QMI_ERR_INVALID_MESSAGE_ID 57u

/* One entry of the modem's registry: a service, the domain that provides it, and the instance that domain
   publishes it under. */
struct fuxi_servreg_entry {
    const char *service;
    const char *domain;
    uint32_t instance;
};

/* The modem's own table, as /image/modemr.jsn names it: one protection domain with the QMI instance 180, and
   two services in it. The names are the ones the modem asks for, "provider/service". */
static const struct fuxi_servreg_entry fuxi_servreg_table[] = {
    {"tms/servreg", "msm/modem/root_pd", 180u},
    {"gps/gps_service", "msm/modem/root_pd", 180u},
};
#define FUXI_SERVREG_ENTRIES (sizeof fuxi_servreg_table / sizeof fuxi_servreg_table[0])

/* The value of the first TLV of the given type in a QMI message, or null when the message has none. The walk
   stops at the message's own length, so a truncated TLV is not read past the end. */
static const uint8_t *fuxi_servreg_tlv(const uint8_t *m, uint32_t length, uint8_t type, uint32_t *size)
{
    if (!m || length < 7u) return 0;
    for (uint32_t at = 7u; at + 3u <= length;) {
        const uint32_t n = (uint32_t)m[at + 1] | (uint32_t)m[at + 2] << 8;
        if (n > length - at - 3u) return 0;
        if (m[at] == type) {
            if (size) *size = n;
            return m + at + 3u;
        }
        at += 3u + n;
    }
    return 0;
}

/* Appends one TLV to the answer being built. */
static int fuxi_servreg_tlv_put(uint8_t *out, uint32_t cap, uint32_t *at, uint8_t type, const void *value, uint32_t size)
{
    if (!out || !at || size > 0xffffu || *at > cap || cap - *at < 3u + size) return -1;
    out[(*at)++] = type;
    out[(*at)++] = (uint8_t)size;
    out[(*at)++] = (uint8_t)(size >> 8);
    memcpy(out + *at, value, size);
    *at += size;
    return 0;
}

/* Serves one request that arrived at the locator's port.
 *
 * The answer is a QMI response that carries the request's transaction and message id back. A GET_DOMAIN_LIST
 * is answered out of the table: the wanted name is the request's TLV 1, and the answer lists the domains that
 * provide it. Any other message id is answered with a failure and the invalid-message-id error, and with
 * nothing else, so a message this does not know is never answered with a success.
 *
 * The wanted name is copied out and the number of matching domains is returned, so the log can say what the
 * modem asked for.
 *
 * Returns 1 when an answer was built, 0 when there is nothing to answer: a packet that is not a QMI request,
 * or an answer that does not fit. */
static int fuxi_servreg_serve(const uint8_t *req, uint32_t req_length, uint8_t *out, uint32_t cap, uint32_t *length,
                              char *wanted, uint32_t wanted_cap, uint32_t *found)
{
    if (!req || !out || !length || req_length < 7u || cap < 7u) return 0;
    /* A response, not a request: there is nothing to answer. */
    if (req[0] != 0x00u) return 0;
    const uint32_t message = (uint32_t)req[3] | (uint32_t)req[4] << 8;
    if (wanted && wanted_cap) wanted[0] = 0;
    if (found) *found = 0;

    uint32_t at = 7u;
    /* The answer's header: the request's own bytes, with the type turned into a response. The transaction and
       the message id are echoed, so the modem can match the answer to its request. */
    memcpy(out, req, 7u);
    out[0] = 0x02u;

    if (message != FUXI_SERVREG_GET_DOMAIN_LIST) {
        /* A message this does not know: a failure and the invalid-message-id error, and nothing else. */
        const uint8_t failed[4] = {FUXI_QMI_RESULT_FAILURE, 0, (uint8_t)FUXI_QMI_ERR_INVALID_MESSAGE_ID,
                                   (uint8_t)(FUXI_QMI_ERR_INVALID_MESSAGE_ID >> 8)};
        if (fuxi_servreg_tlv_put(out, cap, &at, 0x02u, failed, sizeof failed)) return 0;
        out[5] = (uint8_t)(at - 7u);
        out[6] = (uint8_t)((at - 7u) >> 8);
        *length = at;
        return 1;
    }

    /* The name the modem asks for: the request's TLV 1, a plain string. */
    uint32_t wanted_length = 0;
    const uint8_t *name = fuxi_servreg_tlv(req, req_length, 1u, &wanted_length);
    if (wanted && wanted_cap) {
        const uint32_t copy = wanted_length < wanted_cap - 1u ? wanted_length : wanted_cap - 1u;
        if (name && copy) memcpy(wanted, name, copy);
        wanted[copy] = 0;
    }

    /* The list the answer carries: a byte of count, then for each domain a byte of name length, the name
       without a trailing NUL, the instance (four bytes, little endian), the validity byte and the service
       data (four bytes, little endian). */
    uint8_t list[200];
    memset(list, 0, sizeof list);
    uint32_t list_at = 1u;
    uint32_t domains = 0;
    /* A request may ask for the list from an index, and the index is the request's TLV 0x10. The count that is
       answered is the number of domains the name matches, whether they were listed or skipped. */
    uint32_t offset = 0;
    const uint8_t *start = fuxi_servreg_tlv(req, req_length, 0x10u, &wanted_length);
    if (start && wanted_length == 4u) {
        offset = (uint32_t)start[0] | (uint32_t)start[1] << 8 | (uint32_t)start[2] << 16 | (uint32_t)start[3] << 24;
    }
    for (uint32_t i = 0; i < FUXI_SERVREG_ENTRIES; ++i) {
        const struct fuxi_servreg_entry *entry = &fuxi_servreg_table[i];
        const uint32_t service_length = (uint32_t)strlen(entry->service);
        if (!name || service_length != wanted_length || memcmp(entry->service, name, wanted_length)) continue;
        domains++;
        if (domains <= offset) continue;
        const uint32_t domain_length = (uint32_t)strlen(entry->domain);
        if (list_at + 1u + domain_length + 9u > sizeof list) return 0;
        list[list_at++] = (uint8_t)domain_length;
        memcpy(list + list_at, entry->domain, domain_length);
        list_at += domain_length;
        for (uint32_t b = 0; b < 4u; ++b) list[list_at++] = (uint8_t)(entry->instance >> (8u * b));
        list[list_at++] = 0;   /* no service data */
        for (uint32_t b = 0; b < 4u; ++b) list[list_at++] = 0;
        list[0]++;
    }
    if (found) *found = domains;

    /* The result: success, and no error. */
    const uint8_t ok[4] = {FUXI_QMI_RESULT_SUCCESS, 0, 0, 0};
    if (fuxi_servreg_tlv_put(out, cap, &at, 0x02u, ok, sizeof ok)) return 0;
    /* The number of domains, and the table's revision, which is 1 and not the table's own version. */
    const uint8_t count[2] = {(uint8_t)domains, (uint8_t)(domains >> 8)};
    if (fuxi_servreg_tlv_put(out, cap, &at, 0x10u, count, sizeof count)) return 0;
    const uint8_t revision[2] = {1, 0};
    if (fuxi_servreg_tlv_put(out, cap, &at, 0x11u, revision, sizeof revision)) return 0;
    if (domains && fuxi_servreg_tlv_put(out, cap, &at, 0x12u, list, list_at)) return 0;
    out[5] = (uint8_t)(at - 7u);
    out[6] = (uint8_t)((at - 7u) >> 8);
    *length = at;
    return 1;
}

#endif
