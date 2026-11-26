/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* fuxi 2026-11-19: upstream ShengBattery.cpp (with ShengChargeAuth) shares its PMIC GLINK channel with sheng's QcomDwc3 UCSI client.
 * fuxi has no QcomDwc3: these answer "nothing to send, no connection", so the battery manager and the charge authentication run as on
 * sheng without the USB-C owner. Strong definitions: an undefined weak import fails the kernel link here. */
#include <stdint.h>
#include <stdbool.h>
void qcom_dwc3_vbus(int on) {(void)on;}
uint32_t qcom_ucsi_start(uint8_t *out) {(void)out; return 0;}
uint32_t qcom_ucsi_receive(const uint8_t *in, uint32_t length, uint8_t *out) {(void)in; (void)length; (void)out; return 0;}
uint32_t qcom_ucsi_tick(uint8_t *out) {(void)out; return 0;}
bool qcom_ucsi_polling(void) {return false;}
uint32_t qcom_ucsi_request_data_host(uint8_t *out) {(void)out; return 0;}
int qcom_ucsi_data_host(void) {return 0;}
bool qcom_ucsi_connection(uint32_t *a, uint32_t *b) {if (a) *a = 0; if (b) *b = 0; return false;}
