// SPDX-License-Identifier: BSD-2-Clause-Patent
// PMK8550 gpio3 (the WCN7850 32 kHz sleep clock pin, PPID 0x0ba) as UniXNU-sheng's pmicPrepare sets it: pull disabled, power source 0,
// function func1 (source 2), output mode, master enable, in that order. Each register is read, changed only in its own field (masked
// read-modify-write) and read back. A transaction whose status reports failed or dropped is retried a bounded number of times; a denied
// one is not repeated. Pure callback code so a host test can drive it against a model of the arbiter.
#ifndef FUXI_WIFI_CLOCK_H
#define FUXI_WIFI_CLOCK_H
#include <stdint.h>

// arbiter channel status: bit 0 done, bit 1 failed, bit 2 denied, bit 3 dropped
enum {FUXI_CLK_STATUS_DENIED = 1u << 2, FUXI_CLK_TRIES = 4};
// results: 0 written and verified, 1 already an enabled func1 output (nothing written); negative: the step that failed
enum {FUXI_CLK_WRITTEN = 0, FUXI_CLK_ALREADY = 1, FUXI_CLK_READ = -1, FUXI_CLK_DENIED = -2, FUXI_CLK_WRITE = -3, FUXI_CLK_VERIFY = -4};

typedef struct FuxiWifiClockIo {
  void *ctx;
  // one byte through the arbiter: return 0 on success, the raw channel status in *status either way
  int (*read)(void *ctx, uint8_t reg, uint8_t *value, uint32_t *status);
  int (*write)(void *ctx, uint8_t reg, uint8_t value, uint32_t *status);
  // one line per register attempt (may be null)
  void (*trace)(void *ctx, uint8_t reg, uint8_t before, uint8_t wanted, uint8_t after, unsigned attempt, int writeRc, int readRc,
                uint32_t writeStatus, uint32_t readStatus);
  void (*delay)(void *ctx);
} FuxiWifiClockIo;

static inline int fuxi_wifi_clock_read(const FuxiWifiClockIo *io, uint8_t reg, uint8_t *value, uint32_t *status) {
  int rc = 1;
  for (unsigned i = 0; i < FUXI_CLK_TRIES; ++i) {
    *status = 0;
    rc = io->read(io->ctx, reg, value, status);
    if (!rc || (*status & FUXI_CLK_STATUS_DENIED)) break;
    if (io->delay) io->delay(io->ctx);
  }
  return rc;
}

// The pin's mode (0x40 bits 1:0 = 1 output), source (0x44 bits 3:0 = 2 func1) and enable (0x46 bit 7).
static inline int fuxi_wifi_clock_ready(uint8_t mode, uint8_t source, uint8_t enable) {
  return (enable & 0x80) && (mode & 3) == 1 && (source & 0xf) == 2;
}

static inline int fuxi_wifi_sleep_clock(const FuxiWifiClockIo *io) {
  static const uint8_t regs[5] = {0x42, 0x41, 0x44, 0x40, 0x46}, values[5] = {5, 0, 2, 1, 0x80}, masks[5] = {0x07, 0x07, 0x8f, 0x03, 0x80};
  uint8_t mode = 0, source = 0, enable = 0;
  uint32_t status = 0;
  if (fuxi_wifi_clock_read(io, 0x40, &mode, &status) || fuxi_wifi_clock_read(io, 0x44, &source, &status) ||
      fuxi_wifi_clock_read(io, 0x46, &enable, &status))
    return (status & FUXI_CLK_STATUS_DENIED) ? FUXI_CLK_DENIED : FUXI_CLK_READ;
  if (fuxi_wifi_clock_ready(mode, source, enable)) return FUXI_CLK_ALREADY;
  for (unsigned r = 0; r < 5; ++r) {
    int done = 0;
    for (unsigned attempt = 0; attempt < FUXI_CLK_TRIES && !done; ++attempt) {
      uint8_t before = 0, after = 0xff;
      uint32_t ws = 0, rs = 0;
      int rr = fuxi_wifi_clock_read(io, regs[r], &before, &rs);
      if (rr) {
        if (io->trace) io->trace(io->ctx, regs[r], 0, values[r], 0, attempt, 0, rr, 0, rs);
        return (rs & FUXI_CLK_STATUS_DENIED) ? FUXI_CLK_DENIED : FUXI_CLK_READ;
      }
      const uint8_t wanted = (uint8_t)((before & ~masks[r]) | (values[r] & masks[r]));
      int wr = 0;
      if (before != wanted) wr = io->write(io->ctx, regs[r], wanted, &ws);
      if (!wr) rr = fuxi_wifi_clock_read(io, regs[r], &after, &rs);
      if (io->trace) io->trace(io->ctx, regs[r], before, wanted, after, attempt, wr, rr, ws, rs);
      if ((ws | rs) & FUXI_CLK_STATUS_DENIED) return FUXI_CLK_DENIED;
      done = !wr && !rr && (after & masks[r]) == (wanted & masks[r]);
      if (!done && io->delay) io->delay(io->ctx);
    }
    if (!done) return FUXI_CLK_WRITE;
  }
  if (fuxi_wifi_clock_read(io, 0x40, &mode, &status) || fuxi_wifi_clock_read(io, 0x44, &source, &status) ||
      fuxi_wifi_clock_read(io, 0x46, &enable, &status))
    return FUXI_CLK_READ;
  return fuxi_wifi_clock_ready(mode, source, enable) ? FUXI_CLK_WRITTEN : FUXI_CLK_VERIFY;
}
#endif
