/*
 * dlisa.tfa9873.flags.h on the host: what the TFA9873's status registers say (NXP's B0 field names: 0x10 flag_por,
 * flag_bst_ocpok, flag_otpok, flag_ocp_alarm, flag_uvpok, ...; 0x13 flag_ovpok, flag_clip; 0x16 temp_adc), and when the
 * driver stops the amplifiers.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../dlisa.tfa9873.flags.h"

static int failures;

#define CHECK(what, cond) do { if (!(cond)) { printf("FAIL: %s\n", what); failures++; } } while (0)

int
main(void)
{
	char text[160];

	/* as lisa's amplifiers were found powered down (periph-p3): POR, boost OCP ok, OTP ok, UVP ok */
	CHECK("idle as found is no fault", dlisa_tfa_fault(0x0017, 0x0100, 30) == NULL);
	CHECK("over-temperature (otpok 0)", dlisa_tfa_fault(0x0013, 0x0100, 30) != NULL);
	CHECK("amplifier OCP alarm", dlisa_tfa_fault(0x001f, 0x0100, 30) != NULL);
	CHECK("boost OCP (bst_ocpok 0)", dlisa_tfa_fault(0x0015, 0x0100, 30) != NULL);
	CHECK("over-voltage (ovpok 0)", dlisa_tfa_fault(0x0017, 0x0000, 30) != NULL);
	CHECK("under-voltage alone is no fault (the chip mutes itself; logged)", dlisa_tfa_fault(0x0007, 0x0100, 30) == NULL);
	CHECK("clipping alone is no fault", dlisa_tfa_fault(0x0017, 0x0300, 30) == NULL);
	CHECK("hot die under the limit", dlisa_tfa_fault(0x0017, 0x0100, DLISA_TFA_HOT_C) == NULL);
	CHECK("hot die over the limit", dlisa_tfa_fault(0x0017, 0x0100, DLISA_TFA_HOT_C + 1) != NULL);
	CHECK("names the reason", strstr(dlisa_tfa_fault(0x0013, 0x0100, 30), "temperature") != NULL);

	dlisa_tfa_describe(text, sizeof(text), 0x0017, 0x0000, 0x0100, 0x1e);
	CHECK("describe idle", strstr(text, "por") != NULL && strstr(text, "otp ok") != NULL && strstr(text, "30 C") != NULL);
	dlisa_tfa_describe(text, sizeof(text), 0x0049, 0x6040, 0x0300, 0x2a);
	CHECK("describe playing", strstr(text, "clocks") != NULL && strstr(text, "pll") != NULL &&
	    strstr(text, "amp on") != NULL && strstr(text, "CLIP") != NULL && strstr(text, "OCP") != NULL &&
	    strstr(text, "UVP") != NULL);
	CHECK("describe fits", strlen(text) < sizeof(text));

	CHECK("temperature is 9 bits", dlisa_tfa_temperature(0xfe1e) == 0x1e);
	CHECK("battery is 10 bits", dlisa_tfa_battery(0xfc00 | 0x2a5) == 0x2a5);

	printf("%s\n", failures ? "tfa flags: FAILURES" : "tfa flags: all passed");
	return failures != 0;
}
