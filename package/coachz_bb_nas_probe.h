/* fuxi 2026-11-26: from ljhhkx529/QcomBaseband fuxi 0b7f150 drivers/QcomBaseband/baseband/coachz_bb_nas_probe.h. */
/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Read-only requests to the modem's radio services: is the radio on, has it found a network, how strong is it.
 *
 * The card is read (coachz_bb_uim_probe.h) and the card is there. What is missing is the other half of the goal:
 * whether the radio has registered, and how strong the signal is. The modem offers the three services that answer
 * that, but nothing asks them: the bridge only forwards what CommCenter requests, and CommCenter's own registration
 * path runs through the Apple private services, which this modem does not have.
 *
 * Three requests, all read-only, none of which changes anything:
 *
 *   DMS Get Operating Mode (0x2d, service 0x02): the radio's power state. A radio that is offline has no signal
 *   to report, so this is asked first and its answer says whether the other two are worth asking.
 *   NAS Get Serving System (0x24, service 0x03): the registration state and the network the radio is on.
 *   NAS Get Signal Info (0x4f, service 0x03): the signal strength, the RSSI and the SNR.
 *
 * None of them takes an input TLV, none of them writes to the card and none of them changes the radio's state.
 * The answers are read by type and length, and the values are printed as they came, so the bytes settle what the
 * numbers mean.
 */
#ifndef COACHZ_BB_NAS_PROBE_H
#define COACHZ_BB_NAS_PROBE_H

#include <stdint.h>

#define CBN_SVC_DMS 0x02u
#define CBN_SVC_NAS 0x03u
/* DMS Get Operating Mode. */
#define CBN_DMS_GET_OPERATING_MODE 0x002du
/* NAS Get Serving System and Get Signal Info. */
#define CBN_NAS_GET_SERVING_SYSTEM 0x0024u
#define CBN_NAS_GET_SIGNAL_INFO 0x004fu
/* NAS: the two messages that make the modem register, and the one that says how the network is chosen. */
#define CBN_NAS_INITIATE_NETWORK_REGISTER 0x0022u
#define CBN_NAS_GET_SYSTEM_SELECTION 0x0034u
#define CBN_NAS_GET_SYSTEM_INFO 0x004du
/* NAS Initiate Network Register: the register action TLV (1 = automatic). */
#define CBN_TLV_REGISTER_ACTION 0x01u
/* NAS Get System Selection Preference: the mode preference. */
#define CBN_TLV_SELECTION_MODE 0x11u
/* The result TLV, the same one the SSCTL and UIM answers carry. */
#define CBN_TLV_RESULT 0x02u
/* DMS: the operating mode. */
#define CBN_TLV_OPERATING_MODE 0x01u
/* NAS Get Serving System: the serving system, and the network it is on. */
#define CBN_TLV_SERVING_SYSTEM 0x01u
#define CBN_TLV_CURRENT_PLMN 0x12u
/* NAS Get Signal Info: the LTE signal strength. The modem on this board answers the four-byte form (0x17,
   RSRP then RSRQ), which is the one the UMI port parses first too (hakios.nas.c). */
#define CBN_TLV_LTE_SIGNAL 0x14u
#define CBN_TLV_LTE_SIGNAL_ALT 0x17u
#define CBN_TLV_LTE_SIGNAL_2 0x18u
/* NAS Get System Info: the LTE service status. */
#define CBN_TLV_LTE_SERVICE_STATUS 0x14u

/* The operating mode (Qualcomm's dms_service_v01.h). */
enum cbn_operating_mode {
    /* Qualcomm's dms_service_v01.h, and the same numbers the UMI port names: 0 online, 1 low power,
       3 offline, 6 persistent low power, 7 mode-only low power. The earlier table had these shifted by one,
       which made an online modem print as "offline" and sent the search for the missing service in the wrong
       direction. */
    CBN_MODE_ONLINE = 0,
    CBN_MODE_LOW_POWER = 1,
    CBN_MODE_FACTORY_TEST = 2,
    CBN_MODE_OFFLINE = 3,
    CBN_MODE_SHUTTING_DOWN = 4,
    CBN_MODE_LOW_POWER_ONLY = 6,
    CBN_MODE_PERSIST = 7,
};

/* The registration state (libqmi's QmiNasRegistrationState). */
enum cbn_registration_state {
    CBN_REG_NOT_REGISTERED = 0,
    CBN_REG_REGISTERED = 1,
    CBN_REG_NOT_REGISTERED_SEARCHING = 2,
    CBN_REG_REGISTRATION_DENIED = 3,
    CBN_REG_UNKNOWN = 4,
};

/* The state of one request. */
struct cbn_probe {
    uint8_t sent;        /* the request went out */
    uint8_t answered;    /* an answer came back */
    uint32_t error;      /* the result the modem reported */
    uint8_t tlv_types[16];
    uint8_t tlv_lens[16];
    uint32_t tlv_count;
};

/* The state of the radio, gathered from the three answers. */
struct cbn_radio {
    uint8_t operating_mode;
    uint8_t have_operating_mode;
    uint8_t reg_state;
    uint8_t cs_attached;
    uint8_t ps_attached;
    uint8_t have_serving;
    uint16_t mcc;
    uint16_t mnc;
    uint8_t have_plmn;
    int8_t rssi;
    int8_t rsrq;
    int16_t rsrp;
    int16_t snr;
    uint8_t have_signal;
};

/* Builds the request: a QMI request SDU with no TLVs. All three messages take none. */
static inline uint32_t cbn_build_request(uint8_t *sdu, uint32_t room, uint32_t txn, uint32_t message)
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

/* Reads the result and the list of TLVs the answer carried. Returns 1 when the answer was a response.
 *
 * The result TLV is type 0x02 and carries the error code in the last two bytes of its value (the same layout the
 * bridge's own TLV writer uses and the same place the SSCTL reader takes it from). The TLVs of a QMI answer start
 * at byte 7, each one a type byte, a 16-bit little-endian length and then the value.
 *
 * A TLV whose length runs past the end of the answer ends the walk rather than failing the whole answer: the TLVs
 * before it were read and they are worth having, and the raw bytes are on the log either way. */
static inline int cbn_read_answer(struct cbn_probe *st, const uint8_t *sdu, uint32_t n)
{
    if (n < 7u || sdu[0] != 0x02u) return 0;   /* 0x02 is a response */
    const uint32_t end = 7u + (uint32_t)(sdu[5] | (sdu[6] << 8));
    if (end > n) return 0;
    st->error = 0;
    for (uint32_t at = 7u; at + 3u <= end;) {
        const uint32_t type = sdu[at];
        const uint32_t len = sdu[at + 1] | (sdu[at + 2] << 8);
        if (at + 3u + len > end) break;
        if (type == CBN_TLV_RESULT && len >= 2u) {
            const uint8_t *value = sdu + at + 3;
            st->error = (uint32_t)(value[len - 2] | value[len - 1] << 8);
        }
        if (st->tlv_count < 16u) {
            st->tlv_types[st->tlv_count] = (uint8_t)type;
            st->tlv_lens[st->tlv_count] = (uint8_t)(len > 255u ? 255u : len);
            st->tlv_count++;
        }
        at += 3u + len;
    }
    return 1;
}

/* Finds one TLV in an answer: its value and its length, or 0 when the answer does not carry it.
 * The TLVs start at byte 7; a TLV is a type byte, a 16-bit little-endian length and then the value. */
static inline const uint8_t *cbn_find_tlv(const uint8_t *sdu, uint32_t n, uint32_t type, uint32_t *len)
{
    if (!sdu || n < 7u) return 0;
    const uint32_t end = 7u + (uint32_t)(sdu[5] | (sdu[6] << 8));
    if (end > n) return 0;
    for (uint32_t at = 7u; at + 3u <= end;) {
        const uint32_t t = sdu[at];
        const uint32_t l = sdu[at + 1] | (sdu[at + 2] << 8);
        if (at + 3u + l > end) break;
        if (t == type) { *len = l; return sdu + at + 3; }
        at += 3u + l;
    }
    *len = 0;
    return 0;
}

/* Takes the operating mode out of a DMS Get Operating Mode answer. */
static inline int cbn_take_operating_mode(struct cbn_radio *r, const uint8_t *sdu, uint32_t n)
{
    uint32_t len = 0;
    const uint8_t *v = cbn_find_tlv(sdu, n, CBN_TLV_OPERATING_MODE, &len);
    if (!v || len < 1u) return 0;
    r->operating_mode = v[0];
    r->have_operating_mode = 1;
    return 1;
}

/* Takes the registration state and the network out of a NAS Get Serving System answer.
 * The serving system is TLV 0x01 {registration state, CS attach, PS attach, selected network, radio interfaces},
 * and the network is TLV 0x12 {MCC, MNC, the name}. */
static inline int cbn_take_serving_system(struct cbn_radio *r, const uint8_t *sdu, uint32_t n)
{
    uint32_t len = 0;
    const uint8_t *v = cbn_find_tlv(sdu, n, CBN_TLV_SERVING_SYSTEM, &len);
    if (!v || len < 3u) return 0;
    r->reg_state = v[0];
    r->cs_attached = v[1];
    r->ps_attached = v[2];
    r->have_serving = 1;
    const uint8_t *plmn = cbn_find_tlv(sdu, n, CBN_TLV_CURRENT_PLMN, &len);
    if (plmn && len >= 4u) {
        r->mcc = (uint16_t)(plmn[0] | plmn[1] << 8);
        r->mnc = (uint16_t)(plmn[2] | plmn[3] << 8);
        r->have_plmn = 1;
    }
    return 1;
}

/* Takes the signal strength out of a NAS Get Signal Info answer.
 * The LTE signal strength is TLV 0x14 {RSSI, RSRQ, RSRP, SNR} on some builds and TLV 0x17 {RSRP, RSRQ} on this
 * modem, which is the form the UMI port reads (hakios.nas.c: type 0x17, level = the first 16-bit word). The 0x18
 * TLV is the second half the modem sends beside it. */
static inline int cbn_take_signal(struct cbn_radio *r, const uint8_t *sdu, uint32_t n)
{
    uint32_t len = 0;
    const uint8_t *v = cbn_find_tlv(sdu, n, CBN_TLV_LTE_SIGNAL_ALT, &len);
    if (v && len >= 2u) {
        r->rsrp = (int16_t)(v[0] | v[1] << 8);
        r->rsrq = len >= 4u ? (int16_t)(v[2] | v[3] << 8) : 0;
        r->rssi = (int8_t)(r->rsrp);
        r->have_signal = 1;
        return 1;
    }
    v = cbn_find_tlv(sdu, n, CBN_TLV_LTE_SIGNAL, &len);
    if (!v || len < 6u) return 0;
    r->rssi = (int8_t)v[0];
    r->rsrq = (int8_t)v[1];
    r->rsrp = (int16_t)(v[2] | v[3] << 8);
    r->snr = (int16_t)(v[4] | v[5] << 8);
    r->have_signal = 1;
    return 1;
}

#endif
