/*
 * darwin-lisa: lisa's two NXP TFA9873 speaker amplifiers (dlisa.tfa9873.c), for the speakers' path (dlisa.sound.c).
 * Every call that writes the chips is made from a thread that may sleep, except dlisa_tfa_quiesce().
 */
#ifndef _DLISA_TFA9873_H_
#define _DLISA_TFA9873_H_

#include <stdbool.h>
#include <stdint.h>

#define DLISA_TFA_TOP           1u      /* 0x34, pmOS "Earpiece", I2S channel 0 (left): the top unit, also the receiver */
#define DLISA_TFA_BOTTOM        2u      /* 0x35, pmOS "Speaker", I2S channel 1 (right): the loudspeaker */

/*
 * boot-args sound_amps (mask of the two above, default both), sound_amp_reset=1, sound_dcdc=0|1 (default 1),
 * sound_tune=0|1|2 (default 1): 0 pmOS's registers only; 1 also Xiaomi's: NXP's optimal settings for revision 0x0b73
 * (tfa9873_specific) and the stock container's limits and boost (dlisa.tfa9873.tuning.h); 2 also the container's gain
 */
void dlisa_tfa_configure(uint32_t mask, bool pulse_reset, bool dcdc, uint32_t tune);
bool dlisa_tfa_look(void);              /* the pins logged, each chip's revision read; nothing written (I2C reads only) */
bool dlisa_tfa_prepare(void);           /* soft reset and pmOS's register setup; ends muted and powered down */
bool dlisa_tfa_start(void);             /* powered up and unmuted: only while the MI2S clock runs */
bool dlisa_tfa_healthy(void);           /* both still answer and hold what was written; on anything else both are stopped */
void dlisa_tfa_stop(void);              /* muted, then powered down */
void dlisa_tfa_quiesce(void);           /* powered down at once, no lock, no sleep: a panic's way out */
bool dlisa_tfa_on(void);
const char *dlisa_tfa_state(void);      /* one line for kern.hakios.sound_status */

#endif /* _DLISA_TFA9873_H_ */
