/*
 * darwin-lisa: what the TFA9873's status registers say, and when the amplifiers are stopped (dlisa.tfa9873.c).
 * Field names and positions: NXP's tfa9873_tfafieldnames_B0.h (Xiaomi lisa-r-oss): register 0x10 flag_por (bit 0),
 * flag_bst_ocpok (1), flag_otpok (2), flag_ocp_alarm (3), flag_uvpok (4), flag_man_alarm_state (5), flag_clocks_stable
 * (6), flag_mtp_busy (7), flag_lost_clk (8), flag_tdm_error (10); 0x11 flag_bst_bstcur (0), flag_pll_lock (6),
 * flag_engage (13), flag_enbl_amp (14), flag_enbl_ref (15); 0x13 flag_ovpok (8), flag_clip (9); 0x15 bat_adc (bits 0..9);
 * 0x16 temp_adc (bits 0..8, degrees C). The "ok" flags are 1 when all is well (lisa's chips read 0x10 = 0x0017 idle).
 * Pure: the host test (test/tfa_flags_test.c) includes it after <stdio.h>, the driver after libkern.
 */
#ifndef _DLISA_TFA9873_FLAGS_H_
#define _DLISA_TFA9873_FLAGS_H_

#define DLISA_TFA_HOT_C         100u    /* the die's own over-temperature protection is higher; this stops earlier */

#define TFA_F0_POR              (1u << 0)
#define TFA_F0_BST_OCPOK        (1u << 1)
#define TFA_F0_OTPOK            (1u << 2)
#define TFA_F0_OCP_ALARM        (1u << 3)
#define TFA_F0_UVPOK            (1u << 4)
#define TFA_F0_MAN_ALARM        (1u << 5)
#define TFA_F0_CLOCKS           (1u << 6)
#define TFA_F0_MTP_BUSY         (1u << 7)
#define TFA_F0_LOST_CLK         (1u << 8)
#define TFA_F0_TDM_ERROR        (1u << 10)
#define TFA_F1_CURRENT_LIMIT    (1u << 0)
#define TFA_F1_PLL              (1u << 6)
#define TFA_F1_ENGAGE           (1u << 13)
#define TFA_F1_AMP              (1u << 14)
#define TFA_F1_REFS             (1u << 15)
#define TFA_F3_OVPOK            (1u << 8)
#define TFA_F3_CLIP             (1u << 9)

static inline unsigned
dlisa_tfa_temperature(uint16_t reg16)
{
	return reg16 & 0x1ffu;
}

static inline unsigned
dlisa_tfa_battery(uint16_t reg15)
{
	return reg15 & 0x3ffu;
}

/* NULL, or why the amplifiers must stop: over-temperature, over-current (amplifier or boost), over-voltage, too hot */
static inline const char *
dlisa_tfa_fault(uint16_t f0, uint16_t f3, unsigned temperature)
{
	const char *why = NULL;

	why = (f0 & TFA_F0_OTPOK) == 0 ? "over-temperature (otpok 0)" : why;
	why = why == NULL && (f0 & TFA_F0_OCP_ALARM) != 0 ? "amplifier over-current (ocp alarm)" : why;
	why = why == NULL && (f0 & TFA_F0_BST_OCPOK) == 0 ? "boost over-current (bst_ocpok 0)" : why;
	why = why == NULL && (f3 & TFA_F3_OVPOK) == 0 ? "over-voltage (ovpok 0)" : why;
	why = why == NULL && temperature > DLISA_TFA_HOT_C ? "die temperature over the driver's limit" : why;
	return why;
}

/* one line: the flags that are set (or, for the "ok" ones, missing), the temperature */
static inline void
dlisa_tfa_describe(char *out, size_t size, uint16_t f0, uint16_t f1, uint16_t f3, uint16_t reg16)
{
	static const struct {
		int reg;
		uint16_t bit;
		int wanted;             /* 1: said when set; 0: said when clear */
		const char *text;
	} words[] = {
		{ 0, TFA_F0_POR, 1, "por" }, { 0, TFA_F0_OTPOK, 1, "otp ok" }, { 0, TFA_F0_OTPOK, 0, "OTP" },
		{ 0, TFA_F0_BST_OCPOK, 0, "BOOST-OCP" }, { 0, TFA_F0_OCP_ALARM, 1, "OCP" }, { 0, TFA_F0_UVPOK, 0, "UVP" },
		{ 0, TFA_F0_MAN_ALARM, 1, "alarm" }, { 0, TFA_F0_CLOCKS, 1, "clocks" }, { 0, TFA_F0_MTP_BUSY, 1, "mtp-busy" },
		{ 0, TFA_F0_LOST_CLK, 1, "LOST-CLOCK" }, { 0, TFA_F0_TDM_ERROR, 1, "TDM-ERROR" },
		{ 1, TFA_F1_CURRENT_LIMIT, 1, "current-limiting" }, { 1, TFA_F1_PLL, 1, "pll" },
		{ 1, TFA_F1_ENGAGE, 1, "engaged" }, { 1, TFA_F1_AMP, 1, "amp on" }, { 1, TFA_F1_REFS, 1, "refs" },
		{ 3, TFA_F3_OVPOK, 0, "OVP" }, { 3, TFA_F3_CLIP, 1, "CLIP" },
	};
	size_t at = 0;

	out[0] = 0;
	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]) && at + 1 < size; i++) {
		uint16_t reg = words[i].reg == 0 ? f0 : words[i].reg == 1 ? f1 : f3;
		int set = (reg & words[i].bit) != 0;
		int n;

		if (set != words[i].wanted) {
			continue;
		}
		n = snprintf(out + at, size - at, "%s%s", at ? ", " : "", words[i].text);
		at += n > 0 ? (size_t)n : 0;
	}
	if (at + 1 < size) {
		snprintf(out + at, size - at, "%s%u C", at ? "; " : "", dlisa_tfa_temperature(reg16));
	}
}

#endif /* _DLISA_TFA9873_FLAGS_H_ */
