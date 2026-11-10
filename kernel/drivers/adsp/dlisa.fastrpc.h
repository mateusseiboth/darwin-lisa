/*
 * darwin-lisa: the sensors PD's FastRPC file server (dlisa.fastrpc.c, boot-arg sensors_rpc=1), as hakios.adsp.c sees
 * it. kernel/drivers/adsp/sensors-wire.py puts this header beside hakios.adsp.c in the tree; hakios.adsp.c answers the
 * DSP's channel "fastrpcglink-apps-dsp" and calls these only when it finds this header (__has_include), so a tree
 * wired by adsp-wire.py alone builds as before.
 */
#ifndef DLISA_FASTRPC_H
#define DLISA_FASTRPC_H
#define DLISA_FASTRPC 1

bool dlisa_fastrpc_wanted(void);                                /* sensors_rpc=1 */
void dlisa_fastrpc_input(const uint8_t *data, uint32_t length); /* a message of the DSP's on the channel (link's thread) */
void dlisa_fastrpc_start(void);                                 /* once the DSP is up and linked */
void dlisa_fastrpc_stop(void);                                  /* before the DSP is stopped */
void dlisa_fastrpc_quiesce(void);                               /* on a panic's way back to the loader: no lock, no sleep */

/* hakios.adsp.c */
bool dlisa_adsp_fastrpc_open(void);
int dlisa_adsp_fastrpc_send(const void *data, uint32_t length);
#endif
