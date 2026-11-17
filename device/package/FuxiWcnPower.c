/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* fuxi (2026-11-15): the WCN7850 rails are brought up and held by the Wi-Fi driver (fuxi-wifi=5, FuxiWifiLink.cpp); Bluetooth only
 * keeps its vote here, so a later shared power-off can respect both radios. Returns positive errno (WcnPower.h). */
#include <stdint.h>
#include "WcnPower.h"
static volatile unsigned fuxi_wcn_owners;
int sheng_wcn_power_get(unsigned radio) {
    if (radio > 1) return 22;
    __atomic_fetch_or(&fuxi_wcn_owners, 1u << radio, __ATOMIC_ACQ_REL);
    return 0;
}
int sheng_wcn_power_put(unsigned radio) {
    if (radio > 1) return 22;
    __atomic_fetch_and(&fuxi_wcn_owners, ~(1u << radio), __ATOMIC_ACQ_REL);
    return 0;
}
unsigned sheng_wcn_power_owners(void) { return __atomic_load_n(&fuxi_wcn_owners, __ATOMIC_ACQUIRE); }
