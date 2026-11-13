/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* fuxi, 2026-11-13: what AudioProbe.cpp needs from outside the iOS 17 kernel. The kernel links no compiler runtime, and AudioProbe's
 * timeline arithmetic (19.2 MHz host ticks per frame in 32.32 fixed point, computed in 128 bits) makes the compiler call __udivti3.
 * Shift and subtract only, so this body never calls itself. sheng_buttons_press (ButtonProbe.cpp on sheng, for the lock sound test at
 * sheng-audio bit 8) has no fuxi counterpart: a plain definition (a weak one became a chained-fixup import the prelinker refuses) that reports "not pressed". */
#include <stdint.h>
#include <stdbool.h>

typedef unsigned __int128 u128;
u128 __udivti3(u128 n, u128 d);
u128 __udivti3(u128 n, u128 d)
{
    if (!d) return 0;   /* never asked for: AudioProbe divides by the host clock rate and a per-frame tick count */
    if ((n >> 64) == 0 && (d >> 64) == 0) return (u128)((uint64_t)n / (uint64_t)d);
    u128 q = 0, r = 0;
    for (int i = 127; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {r -= d; q |= (u128)1 << i;}
    }
    return q;
}

bool sheng_buttons_press(uint32_t mask, uint32_t ms);
bool sheng_buttons_press(uint32_t mask, uint32_t ms)
{
    (void)mask; (void)ms;
    return false;
}
