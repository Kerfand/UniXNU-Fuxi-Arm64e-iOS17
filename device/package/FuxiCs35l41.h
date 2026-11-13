/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The two CS35L41 speaker amplifiers of the fuxi board (Xiaomi 13), 2026-11-13. Stock devicetree: both on QUP2 SE0 (i2c@880000), "B"
 * (bottom) at 0x40 with cirrus,right-channel-amp, reset gpio183, interrupt tlmm 99; "T" (top/receiver) at 0x42, reset gpio182,
 * interrupt tlmm 98; boost 1000 nH, 15 uF, 4000 mA peak; ASP SDOUT high impedance (3); GPIO2 output, source 4. Both take 16-bit stereo
 * I2S from the secondary MI2S (gpio121-124) as clock slaves and play their slot straight from the serial port (DAC_PCM1_SRC = ASPRX1,
 * the amplifier DSP bypassed, no firmware): T slot 0 (left), B slot 1 (right).
 * Registers, fields and sequences: Linux include/sound/cs35l41.h, sound/soc/codecs/cs35l41-lib.c and cs35l41.c (the boot wait, the
 * rev B0/B2 errata, boost_config, set_dai_fmt, pcm_hw_params, set_sysclk with SCLK as PLL reference, the pup/pdn patches and
 * global_enable for internal boost). Not done: OTP trim unpack (cs35l41_otp_unpack) and the DSP set-up (cs35l41_dsp_init).
 * The interface is ShengCs35l43.h's (same names), so AudioProbe.cpp runs these programs with its runner unchanged.
 * Registers and values are 32-bit big-endian on the wire. */
#ifndef FUXI_CS35L41_H
#define FUXI_CS35L41_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CS35_DEVID 0x00000000u
#define CS35_REVID 0x00000004u
#define CS35_OTPID 0x00000010u
#define CS35_SFT_RESET 0x00000020u
#define CS35_TEST_KEY 0x00000040u
#define CS35_PWR_CTRL1 0x00002014u
#define CS35_PWR_CTRL2 0x00002018u
#define CS35_PWR_CTRL3 0x0000201Cu
#define CS35_PROTECT_REL_ERR_IGN 0x00002034u
#define CS35_GPIO_PAD_CONTROL 0x0000242Cu
#define CS35_PWRMGT_STS 0x00002908u
#define CS35_PLL_CLK_CTRL 0x00002C04u
#define CS35_GLOBAL_CLK_CTRL 0x00002C0Cu
#define CS35_TST_FS_MON0 0x00002D10u
#define CS35_BSTCVRT_PEAK_CUR 0x00003808u
#define CS35_BSTCVRT_COEFF 0x00003810u
#define CS35_BSTCVRT_SLOPE_LBST 0x00003814u
#define CS35_BSTCVRT_DCM_CTRL 0x0000381Cu
#define CS35_VIMON_SPKMON_RESYNC 0x00004100u
#define CS35_VPVBST_FS_SEL 0x00004400u
#define CS35_SP_ENABLES 0x00004800u
#define CS35_SP_FORMAT 0x00004808u
#define CS35_SP_HIZ_CTRL 0x0000480Cu
#define CS35_SP_FRAME_RX_SLOT 0x00004820u
#define CS35_SP_RX_WL 0x00004840u
#define CS35_DAC_PCM1_SRC 0x00004C00u
#define CS35_ASP_TX3_SRC 0x00004C28u
#define CS35_ASP_TX4_SRC 0x00004C2Cu
#define CS35_AMP_DIG_VOL_CTRL 0x00006000u
#define CS35_AMP_GAIN_CTRL 0x00006C04u
#define CS35_IRQ1_STATUS1 0x00010010u
#define CS35_IRQ1_STATUS3 0x00010018u
#define CS35_IRQ1_STATUS4 0x0001001Cu
#define CS35_IRQ1_MASK1 0x00010110u
#define CS35_IRQ1_MASK2 0x00010114u
#define CS35_IRQ1_MASK3 0x00010118u
#define CS35_IRQ1_MASK4 0x0001011Cu
#define CS35_DSP1_CCM_CORE_CTRL 0x02BC1000u
#define CS35_DSP1_XM_ACCEL_PL0_PRI 0x02BC2020u
#define CS35_DSP1_YM_ACCEL_PL0_PRI 0x02BC20E0u

#define CS35_CHIP_ID 0x00035A40u
#define CS35_OTP_BOOT_DONE 0x00000002u   /* IRQ1_STATUS4 */
#define CS35_OTP_BOOT_ERR 0x80000000u    /* IRQ1_STATUS3 */
#define CS35_PUP_DONE 0x01000000u        /* IRQ1_STATUS1 */
#define CS35_PDN_DONE 0x00800000u        /* IRQ1_STATUS1 */
/* The status program reads IRQ1_STATUS1 (reads[0]) then IRQ1_STATUS3 (reads[1]); AudioProbe tests these names. */
#define CS35_EINT2_PLL_LOCK 0x00000002u  /* IRQ1_STATUS3 bit 1 */
#define CS35_EINT1_ERRORS 0x800281C0u    /* AMP_SHORT, TEMP_ERR, TEMP_WARN, BST_SHORT, BST_DCM_UVP, BST_OVP */

#define CS35_AMPS 2
/* "Analog PCM Volume" index (0.5 dB + 1 dB per step, at most 20): 17 = 17.5 dB, near sheng's 18 dB */
#define CS35_GAIN_INDEX 17u
/* The I2C address of amplifier n (0 = T at 0x42, slot 0; 1 = B at 0x40, slot 1) */
static inline uint8_t cs35_address(unsigned amp) {return amp == 0 ? 0x42u : 0x40u;}
static inline unsigned cs35_slot(unsigned amp) {return amp == 0 ? 0u : 1u;}

enum {Cs35Write = 1, Cs35Update = 2, Cs35Read = 3, Cs35DelayMs = 4};
typedef struct {uint8_t op; uint32_t reg, mask, value;} Cs35Step;

/* Programs (each returns its step count, at most CS35_MAX_STEPS):
 * init: after the reset line was released - boot wait, identity (reads: DEVID, REVID, IRQ1_STATUS4, IRQ1_STATUS3), errata, interrupt
 *       masks, boost, serial port, PLL, routing and gain for the slot; leaves the amplifier powered down.
 * power up / power down: ASP RX1, the pup/pdn patch, global enable (reads: IRQ1_STATUS1), the amplifier enable.
 * status: IRQ1_STATUS1 and IRQ1_STATUS3, then acknowledges both. */
#define CS35_MAX_STEPS 48
unsigned cs35_init_program(Cs35Step *out, unsigned slot);
unsigned cs35_power_up_program(Cs35Step *out);
unsigned cs35_power_down_program(Cs35Step *out);
unsigned cs35_status_program(Cs35Step *out);

typedef struct {
    const Cs35Step *steps;
    unsigned count, index;
    uint8_t phase;
    uint32_t pending;
    uint32_t reads[4];
    unsigned read_count;
} Cs35Run;
enum {Cs35Transfer = 1, Cs35Wait = 2, Cs35Finished = 3};
typedef struct {
    uint8_t bytes[8];
    uint32_t length;
    uint32_t read_length;
    uint32_t delay_ms;
} Cs35Action;
void cs35_run_start(Cs35Run *r, const Cs35Step *steps, unsigned count);
int cs35_run_next(Cs35Run *r, Cs35Action *a);
void cs35_run_result(Cs35Run *r, uint32_t value);
static inline uint32_t cs35_wire_value(uint32_t rx) {return (rx >> 24) | ((rx >> 8) & 0xff00u) | ((rx << 8) & 0xff0000u) | (rx << 24);}

#ifdef __cplusplus
}
#endif
#endif
