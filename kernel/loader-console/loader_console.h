/* SPDX-License-Identifier: MIT */
/*
 * Return-to-loader debug channel (see loader_console.c). Lets a UniXNU kernel
 * booted over the HakiOS xnuloader hand its log back to the loader on panic or
 * after loader_return_after seconds, so it is observable with no serial port.
 */
#ifndef LOADER_CONSOLE_H
#define LOADER_CONSOLE_H

void loader_log_putc(char c);		/* append one char to the log the loader reads */
void loader_return(void);		/* collect the log and jump back to the loader */
void loader_bringup_tick(void);		/* IRQ-driven watchdog pet + timed return (no thread_calls) */
void loader_diag_wfi_probe(void);	/* bring-up diag: does the vtimer survive WFI under the hv? */

/* Install the console tees. Call right after PE_kputc / PE_putc are set. */
void loader_console_tap_kputc(void);
void loader_console_tap_putc(void);

#endif /* LOADER_CONSOLE_H */
