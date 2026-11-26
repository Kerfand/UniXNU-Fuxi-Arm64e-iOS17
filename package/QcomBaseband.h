/* fuxi 2026-11-26: from ljhhkx529/QcomBaseband baseband/QcomBaseband.h (282adce). */
/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef QCOM_BASEBAND_H
#define QCOM_BASEBAND_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int coachz_baseband_enabled(void);
/* `send` sends one QMI body from a local QRTR port to a remote node and port. It returns 0 when the body went
   out and a negative value when the link refused it, in which case the bridge keeps the body and tries it again
   on the next poll. A send that is dropped instead of retried loses the request with nothing on the log to say
   so. */
void coachz_baseband_configure(int (*send)(uint32_t, uint32_t, uint32_t, const void *, uint32_t), void (*del_client)(uint32_t));
void coachz_baseband_new_server(uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port);
void coachz_baseband_del_server(uint32_t service, uint32_t node, uint32_t port);
int coachz_baseband_qrtr(uint32_t dst_port, uint32_t src_node, uint32_t src_port, const void *body, uint32_t size);
void coachz_baseband_poll(void);
void coachz_baseband_report(unsigned second);
/* Implemented by the board data adapter; called on the QRTR/IPA owner thread. */
int coachz_modem_data_send(uint32_t mux, const uint8_t *ip, uint32_t length);
void coachz_modem_data_receiver(void (*receiver)(uint32_t mux, const uint8_t *ip, uint32_t length, uint32_t ip_version));
#ifdef __cplusplus
}
#endif
#endif
