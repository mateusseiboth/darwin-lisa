/* SPDX-License-Identifier: MIT */
/*
 * Loader console: route XNU's early console output into a buffer the HakiOS
 * xnuloader can read, so a UniXNU kernel booted over `xl` is observable the same
 * way the HakiOS kernel is. The loader scans the kernel image for a struct with
 * the magic "XNULIVE!" and then polls its log/log_len while the kernel runs
 * (and reads it once more if the kernel hangs). We fill that struct and tee
 * PE_kputc / PE_putc into its buffer. No clean return to the loader is needed to
 * see the log; the loader's live polling captures it.
 */
#ifndef LOADER_CONSOLE_H
#define LOADER_CONSOLE_H

/* Install the tee on the kprintf char sink (PE_kputc). Call right after it is set. */
void loader_console_tap_kputc(void);

/* Install the tee on the printf char sink (PE_putc). Call right after it is set. */
void loader_console_tap_putc(void);

#endif /* LOADER_CONSOLE_H */
