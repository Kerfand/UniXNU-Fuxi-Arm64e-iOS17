/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SM8550_GENI_UART_H
#define SM8550_GENI_UART_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SM8550_UART14_BASE 0x00898000ULL
#define SM8550_QUP2_BASE 0x008c0000ULL
#define SM8550_UART14_INTID 493u
#define SM8550_QUP2_SID 0x423u
struct sm8550_geni_clock_io {
    void *context;
    uint32_t (*read)(void *,uint32_t);
    void (*write)(void *,uint32_t,uint32_t);
    void (*delay_us)(void *,unsigned);
};
/* UART engine is stopped before source changes. GPLL0-even is 300 MHz. */
int sm8550_geni_uart14_source(const struct sm8550_geni_clock_io *,uint32_t baud,uint32_t *hz);
/* Serialize shared GCC votes from worker context. Returns false on allocation failure. */
bool sm8550_gcc_update32(volatile uint32_t *,uint32_t clear,uint32_t set);
/* Own SE6 and its newly acquired shared core/AHB votes. */
bool sm8550_geni_uart14_vote(volatile uint32_t *,bool enabled);
#ifdef __cplusplus
}
#endif
#endif
