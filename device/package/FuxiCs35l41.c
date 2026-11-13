/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* fuxi's CS35L41 programs (FuxiCs35l41.h). The runner is ShengCs35l43.c's. */
#include "FuxiCs35l41.h"

typedef struct {Cs35Step *p; unsigned n;} P;
static void w(P *p, uint32_t reg, uint32_t value) {if (p->n < CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35Write, reg, 0, value};}
static void u(P *p, uint32_t reg, uint32_t mask, uint32_t value) {if (p->n < CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35Update, reg, mask, value & mask};}
static void r(P *p, uint32_t reg) {if (p->n < CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35Read, reg, 0, 0};}
static void d(P *p, uint32_t ms) {if (p->n < CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35DelayMs, 0, 0, ms};}
static void unlock(P *p) {w(p, CS35_TEST_KEY, 0x55u); w(p, CS35_TEST_KEY, 0xaau);}
static void lock(P *p) {w(p, CS35_TEST_KEY, 0xccu); w(p, CS35_TEST_KEY, 0x33u);}

unsigned cs35_init_program(Cs35Step *out, unsigned slot)
{
    P p = {out, 0};
    /* cs35l41_reset_and_boot: software reset, then OTP boot (up to 100 ms; IRQ1_STATUS4 OTP_BOOT_DONE, IRQ1_STATUS3 OTP_BOOT_ERR). The
     * runner cannot poll: one 20 ms wait, and the two status words are read for the log. */
    w(&p, CS35_SFT_RESET, 0x5A000000u);
    d(&p, 20);
    r(&p, CS35_DEVID);
    r(&p, CS35_REVID);
    r(&p, CS35_IRQ1_STATUS4);
    r(&p, CS35_IRQ1_STATUS3);
    /* cs35l41_revb0/revb2_errata_patch (the two are the same), then the DSP core held off */
    unlock(&p);
    w(&p, CS35_VIMON_SPKMON_RESYNC, 0);
    w(&p, 0x00004310u, 0);
    w(&p, CS35_VPVBST_FS_SEL, 0);
    w(&p, CS35_BSTCVRT_DCM_CTRL, 0x00000051u);
    w(&p, CS35_DSP1_YM_ACCEL_PL0_PRI, 0);
    w(&p, CS35_DSP1_XM_ACCEL_PL0_PRI, 0);
    w(&p, CS35_PWR_CTRL2, 0);
    w(&p, CS35_AMP_GAIN_CTRL, 0);
    w(&p, CS35_ASP_TX3_SRC, 0);
    w(&p, CS35_ASP_TX4_SRC, 0);
    w(&p, CS35_DSP1_CCM_CORE_CTRL, 0);
    lock(&p);
    /* Interrupts stay masked: the owner reads the status registers. */
    w(&p, CS35_IRQ1_MASK1, 0xffffffffu);
    w(&p, CS35_IRQ1_MASK2, 0xffffffffu);
    w(&p, CS35_IRQ1_MASK3, 0xffffffffu);
    w(&p, CS35_IRQ1_MASK4, 0xffffffffu);
    /* cs35l41_boost_config(1000 nH, 15 uF, 4000 mA): K1 0x24, K2 0x24, slope 0x75, LBST 0, IPK 0x40; boost enabled (BST_EN 2) */
    u(&p, CS35_BSTCVRT_COEFF, 0xffffu, 0x24u << 8 | 0x24u);
    u(&p, CS35_BSTCVRT_SLOPE_LBST, 0xff03u, 0x75u << 8);
    u(&p, CS35_BSTCVRT_PEAK_CUR, 0x7fu, (4000u - 1600u) / 50u + 0x10u);
    u(&p, CS35_PWR_CTRL2, 0x30u, 2u << 4);
    /* Board values: SDOUT high impedance (cirrus,asp-sdout-hiz 3), GPIO2 output with source 4 (cirrus,gpio-config2) */
    u(&p, CS35_SP_HIZ_CTRL, 0x3u, 0x3u);
    u(&p, CS35_GPIO_PAD_CONTROL, 0x07000000u, 4u << 24);
    /* set_dai_fmt (I2S, clock and frame from the SoC), hw_params (16-bit, 48 kHz) */
    u(&p, CS35_SP_FORMAT, 0xff000000u | 0x0755u, 16u << 24 | 2u << 8);
    u(&p, CS35_SP_RX_WL, 0x3fu, 16u);
    u(&p, CS35_GLOBAL_CLK_CTRL, 0x1fu, 0x03u);
    /* set_sysclk: the PLL locks to the 1.536 MHz SCLK (reference 0, frequency code 0x1B); FS monitor 52 / 84 */
    u(&p, CS35_PLL_CLK_CTRL, 0x800u, 0x800u);
    u(&p, CS35_PLL_CLK_CTRL, 0x7e0u, 0x1bu << 5);
    u(&p, CS35_PLL_CLK_CTRL, 0x10u, 0);
    u(&p, CS35_PLL_CLK_CTRL, 0x7u, 0);
    u(&p, CS35_PLL_CLK_CTRL, 0x800u, 0);
    u(&p, CS35_PLL_CLK_CTRL, 0x10u, 0x10u);
    w(&p, CS35_TST_FS_MON0, 52u | (84u << 12));
    /* PCM source ASPRX1, this amplifier's slot, analog gain, digital volume 0 dB */
    u(&p, CS35_DAC_PCM1_SRC, 0x7fu, 0x08u);
    u(&p, CS35_SP_FRAME_RX_SLOT, 0x3fu, slot);
    u(&p, CS35_AMP_GAIN_CTRL, 0x3e0u, CS35_GAIN_INDEX << 5);
    u(&p, CS35_AMP_DIG_VOL_CTRL, 0x7ffu << 3, 0);
    return p.n;
}

/* cs35l41_main_amp_event PRE_PMU (pup patch, global enable for internal boost: GLOBAL_EN, PUP_DONE within 100 ms, acknowledge), then the
 * Main AMP bit (PWR_CTRL2 bit 0); ASP RX1 (SP_ENABLES bit 16) first, as DAPM powers the AIF before the output. */
unsigned cs35_power_up_program(Cs35Step *out)
{
    P p = {out, 0};
    u(&p, CS35_SP_ENABLES, 1u << 16, 1u << 16);
    unlock(&p);
    w(&p, 0x00002084u, 0x002F1AA0u);
    lock(&p);
    u(&p, CS35_PWR_CTRL1, 1u, 1u);
    d(&p, 5);
    r(&p, CS35_IRQ1_STATUS1);
    w(&p, CS35_IRQ1_STATUS1, CS35_PUP_DONE);
    u(&p, CS35_PWR_CTRL2, 1u, 1u);
    return p.n;
}

unsigned cs35_power_down_program(Cs35Step *out)
{
    P p = {out, 0};
    u(&p, CS35_PWR_CTRL2, 1u, 0);
    u(&p, CS35_PWR_CTRL1, 1u, 0);
    d(&p, 5);
    r(&p, CS35_IRQ1_STATUS1);
    w(&p, CS35_IRQ1_STATUS1, CS35_PDN_DONE);
    unlock(&p);
    w(&p, 0x00002084u, 0x002F1AA3u);
    lock(&p);
    u(&p, CS35_SP_ENABLES, 1u << 16, 0);
    return p.n;
}

unsigned cs35_status_program(Cs35Step *out)
{
    P p = {out, 0};
    r(&p, CS35_IRQ1_STATUS1);
    r(&p, CS35_IRQ1_STATUS3);
    w(&p, CS35_IRQ1_STATUS1, 0xffffffffu);
    w(&p, CS35_IRQ1_STATUS3, 0xffffffffu);
    return p.n;
}

void cs35_run_start(Cs35Run *r, const Cs35Step *steps, unsigned count)
{
    r->steps = steps; r->count = count; r->index = 0; r->phase = 0; r->read_count = 0; r->pending = 0;
    for (unsigned i = 0; i < 4; ++i) r->reads[i] = 0;
}

static void be32(uint8_t *b, uint32_t v) {b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;}

int cs35_run_next(Cs35Run *r, Cs35Action *a)
{
    a->length = a->read_length = a->delay_ms = 0;
    if (r->index >= r->count) return Cs35Finished;
    const Cs35Step *s = &r->steps[r->index];
    if (r->phase == 4) {
        be32(a->bytes, s->reg); be32(a->bytes + 4, r->pending); a->length = 8;
        r->phase = 2;
        return Cs35Transfer;
    }
    switch (s->op) {
    case Cs35DelayMs:
        a->delay_ms = s->value;
        r->index++;
        return Cs35Wait;
    case Cs35Write:
        be32(a->bytes, s->reg); be32(a->bytes + 4, s->value); a->length = 8;
        r->phase = 2;
        return Cs35Transfer;
    case Cs35Update:
    case Cs35Read:
        be32(a->bytes, s->reg); a->length = 4; a->read_length = 4;
        r->phase = s->op == Cs35Update ? 1 : 3;
        return Cs35Transfer;
    default:
        r->index = r->count;
        return Cs35Finished;
    }
}

void cs35_run_result(Cs35Run *r, uint32_t value)
{
    if (r->index >= r->count) return;
    const Cs35Step *s = &r->steps[r->index];
    if (r->phase == 1) {
        r->pending = (value & ~s->mask) | s->value;
        if (r->pending == value) {r->phase = 0; r->index++;} else r->phase = 4;
        return;
    }
    if (r->phase == 3 && r->read_count < 4) r->reads[r->read_count++] = value;
    r->phase = 0;
    r->index++;
}
