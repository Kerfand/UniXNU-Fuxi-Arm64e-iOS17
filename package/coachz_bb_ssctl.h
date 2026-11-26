/* fuxi 2026-11-26: from ljhhkx529/QcomBaseband baseband/coachz_bb_ssctl.h (282adce). */
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * What the modem serves, and what the SSCTL service is. Portable and host-tested; no kernel code.
 *
 * The name service does not answer this modem. Upstream's name service runs on the AP (Linux net/qrtr/ns.c,
 * ctrl_cmd_new_lookup accepts only local observers), so a lookup the AP sends is a question the modem has no
 * name service to answer. The modem publishes on its own initiative the services it wants announced, and on
 * this board that is two: SERVREG_NOTIF (0x42) and SSCTL (0x2b). UIM (0x0b) is not among them.
 *
 * The list of the modem's services is not an SSCTL question. Qualcomm's own SSCTL header
 * (modem_proc/qmimsgs/ssctl/api/subsystem_control_v01.h) gives the service's whole message set:
 *
 *   0x0020  SSCTL RESTART
 *   0x0021  SSCTL SHUTDOWN
 *   0x0022  SSCTL GET FAILURE REASON
 *   0x0023  SSCTL SUBSYS EVENT
 *
 * There is no "Get Supported Services" in it. An earlier version of this file named
 * QMI_SSCTL_GET_SUPPORTED_SERVICES at id 0x0001 and said libqmi's qmi-service-ssctl.h carried it; libqmi
 * carries no SSCTL service at all, and the message does not exist. The sweep that looked for it therefore
 * asked the modem to restart its own subsystem at id 0x0020 and to shut it down at id 0x0021, and the modem
 * stopped answering because that is what those two messages do. The modem's SMP2P "fatal" bit rose on the
 * modem's own schedule at the same moment in every run, which is the modem reporting a subsystem restart.
 *
 * So the service list is asked of CTL (service 0x00) Get Version Info, message id 0x0021, and CTL under QRTR
 * is a service of the AP itself: libqmi's qmi-endpoint-qrtr.c intercepts the CTL allocate-CID messages and
 * answers them locally, and upstream's net/qrtr/ns.c keeps the server table and answers the lookups. The modem
 * is never asked for a CID, and the modem's own list of its services is not reachable by any CAF message.
 *
 * What is left of SSCTL here is the part that is safe to ask: the failure reason (id 0x0022), which is the
 * modem's own statement of why it stopped, and which is worth having if the modem stops again.
 */
#ifndef COACHZ_BB_SSCTL_H
#define COACHZ_BB_SSCTL_H

#include <stdint.h>

#define CBS_SVC_SSCTL 0x2bu
/* The SSCTL messages, by Qualcomm's own header. The first two are the ones that stop the modem, and they
   are never sent: the bridge has no business restarting or shutting down the modem's subsystem. */
#define CBS_SSCTL_RESTART 0x0020u
#define CBS_SSCTL_SHUTDOWN 0x0021u
#define CBS_SSCTL_GET_FAILURE_REASON 0x0022u
#define CBS_SSCTL_SUBSYS_EVENT 0x0023u
/* The result TLV. */
#define CBS_TLV_RESULT 0x02u
/* The failure reason TLV: the error message as a character array (Qualcomm's subsystem_control_v01.h). */
#define CBS_TLV_FAILURE_MESSAGE 0x10u

/* The state of the one request. */
struct cbq_ssctl {
    uint8_t sent;        /* the request went out */
    uint8_t answered;    /* an answer came back */
    uint32_t error;      /* the result the modem reported */
    /* The failure reason the modem gave: the error message as a character array, and how many bytes of it
       there are. It is the modem's own statement of why it stopped, which is worth more than a guess. */
    uint32_t reason_len;
    char reason[128];
    /* Every TLV the answer carried, whatever its type. The type of the reason is not assumed here, so the
       types the answer actually carried are what settles it: the TLV that is not the result is the one that
       carries the reason. */
    uint8_t tlv_types[16];
    uint8_t tlv_lens[16];
    uint32_t tlv_count;
};

/* Builds the request: a QMI request SDU with no TLVs. Returns its length, 0 when it does not fit.
 *
 * The Get Failure Reason request takes no TLVs (Qualcomm's subsystem_control_v01.h: the request message has
 * no input). The message id is the caller's, so the same builder serves any of the service's messages. */
static inline uint32_t cbs_build_request_id(uint8_t *sdu, uint32_t room, uint32_t txn, uint32_t message)
{
    if (room < 7u) return 0;
    sdu[0] = 0x00u;             /* flags: a request */
    sdu[1] = (uint8_t)txn;     /* transaction */
    sdu[2] = 0x00u;
    sdu[3] = (uint8_t)(message & 0xffu);
    sdu[4] = (uint8_t)(message >> 8);
    sdu[5] = 0x00u;             /* TLV length: no TLVs */
    sdu[6] = 0x00u;
    return 7u;
}

/* The result code the modem gives a message id it does not know (qmi_service_error_v01,
   QMI_SERVICE_ERROR_INVALID_MESSAGE_ID). */
#define CBS_ERR_INVALID_MESSAGE_ID 57u

/* Reads the answer: the result and the failure reason. Returns 1 when the answer was a response.
 *
 * The result TLV is type 0x02 and carries the error code in its last two bytes (the same layout the bridge's
 * own TLV writer uses, cbq_tlv_result). The failure reason is type 0x10 and holds the modem's message as a
 * character array.
 *
 * Every TLV the answer carries is recorded in the state's tlv_types, whatever its type. The type of the reason
 * is not assumed here, so the types the answer actually carried are what settles it: the TLV that is not the
 * result is the one that carries the reason. */
static inline int cbs_read_answer(struct cbq_ssctl *st, const uint8_t *sdu, uint32_t n)
{
    if (n < 7u || 7u + (uint32_t)(sdu[5] | sdu[6] << 8) > n) return 0;
    const uint32_t end = 7u + (uint32_t)(sdu[5] | sdu[6] << 8);
    uint32_t at = 7u;
    while (at < end) {
        if (end - at < 3u || 3u + (uint32_t)(sdu[at + 1] | sdu[at + 2] << 8) > end - at) return 0;
        const uint32_t type = sdu[at];
        const uint32_t len = (uint32_t)(sdu[at + 1] | sdu[at + 2] << 8);
        const uint8_t *value = sdu + at + 3;
        if (st->tlv_count < sizeof st->tlv_types / sizeof st->tlv_types[0]) {
            st->tlv_types[st->tlv_count] = (uint8_t)type;
            st->tlv_lens[st->tlv_count] = (uint8_t)(len > 255u ? 255u : len);
            st->tlv_count++;
        }
        if (type == CBS_TLV_RESULT && len >= 2u) {
            st->error = (uint32_t)(value[len - 2] | value[len - 1] << 8);
        } else if (type == CBS_TLV_FAILURE_MESSAGE) {
            /* The reason is text. It is copied as it is and stopped at the first byte that would not fit; the
               text is the modem's, and it is not trusted to be well formed. */
            const uint32_t room = sizeof st->reason - 1u;
            const uint32_t take = len < room ? len : room;
            for (uint32_t i = 0; i < take; ++i) st->reason[i] = (char)value[i];
            st->reason[take] = 0;
            st->reason_len = take;
        }
        at += 3u + len;
    }
    st->answered = 1;
    return 1;
}

#endif /* COACHZ_BB_SSCTL_H */
