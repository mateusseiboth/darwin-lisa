/*
 * darwin-lisa: stand-ins for the few HakiOS xnuloader symbols that the standard bsd/dev
 * drivers reference but that this xnu-12377 bring-up tree does not provide the same way.
 * Each is non-functional for boot; they only let the drivers link.
 *
 *   loader_log_line  - HakiOS's xnuloader live-log line, used by dev/hakios.input.c's touch
 *                      latency tracing (boot-arg touch_lat=1). No live loader-log channel of
 *                      that shape here, so this is a no-op.
 *   loader_sound     - HakiOS's { phys, bytes, kva } of the audio recording the loader ships
 *                      to the host (dev/hakios.audio.c, boot-arg rec_sound). The tree's
 *                      osfmk/arm64/loader_console.c keeps its OWN static loader_sound[3]; this
 *                      definition only satisfies the driver's extern, it is not read by the
 *                      loader, so on-host sound capture is inert.
 *
 * loader_return() is intentionally absent: osfmk/arm64/loader_console.c already provides it.
 */
#include <stdint.h>

void loader_log_line(const char *line);
void
loader_log_line(const char *line)
{
	(void)line;
}

uint64_t loader_sound[3];
