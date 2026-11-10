/*
 * darwin-lisa: the network the Wi-Fi joins by itself, given by init26 (userland/init/wifi-net.c).
 *
 * hakios.wifi.c joins the network of fw/network when a RAM disk carries one (a host run). A boot from the flash alone
 * has no RAM disk, so init26 keeps that text on the data volume (/private/var/db/dlisa-wifi/network, mode 0600) and
 * writes it here, to kern.hakios.wifi_network, early in every boot with wifi=1. The radio comes up some seconds later
 * (the DSP, then ath11k); a thread waits for its station (sys/hakios.sta.h) and asks for the network as the standing
 * one: joined now, and again whenever the radio is switched back on, until Darwin asks for another.
 *
 * The text is the name on the first line and the passphrase on the second (none: an open network). The sysctl is
 * write-only, masked from listings and root's only; nothing here keeps the text after the request is made, and nothing
 * prints more of it than its lengths.
 */
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/hakios.sta.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern kern_return_t thread_terminate(thread_t thread);

#define TEXT_MOST               128
#define WAIT_FOR_RADIO_SECONDS  300

static LCK_GRP_DECLARE(wifi_net_lock_group, "dlisa-wifi-net");
static LCK_MTX_DECLARE(wifi_net_lock, &wifi_net_lock_group);
static struct hakios_sta_join given;            /* waiting for the radio (ssid_length 0: nothing) */
static bool waiter_runs;

/* The line at TEXT (LENGTH bytes left) without its line end; *next: where the one after it begins */
static size_t
line_length(const char *text, size_t length, size_t *next)
{
	size_t end = 0, line;

	while (end < length && text[end] != '\n') {
		end++;
	}
	line = end > 0 && text[end - 1] == '\r' ? end - 1 : end;
	*next = end < length ? end + 1 : end;
	return line;
}

/* the name (1..32 bytes) and the passphrase (8..63, or none) of TEXT into WHAT; false when TEXT is not that */
static bool
parse_network(const char *text, size_t length, struct hakios_sta_join *what)
{
	size_t after_name = 0, after_passphrase = 0;
	size_t name = line_length(text, length, &after_name);
	size_t passphrase = line_length(text + after_name, length - after_name, &after_passphrase);

	if (name == 0 || name > sizeof(what->ssid) || after_name + after_passphrase != length ||
	    (passphrase != 0 && (passphrase < 8 || passphrase > 63))) {
		return false;
	}
	what->ssid_length = (uint8_t)name;
	memcpy(what->ssid, text, name);
	what->security = passphrase == 0 ? HAKIOS_STA_OPEN : HAKIOS_STA_WPA2_PSK;
	what->passphrase_length = (uint8_t)passphrase;
	memcpy(what->passphrase, text + after_name, passphrase);
	return true;
}

/* the station has that network joined, or is joining it (a RAM disk's fw/network asked for it first) */
static bool
is_station_on(const struct hakios_sta_join *what)
{
	struct hakios_sta_state *state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO);
	bool same;

	if (state == NULL) {
		return false;
	}
	hakios_sta_state(state);
	same = (state->phase == HAKIOS_STA_JOINED || state->phase == HAKIOS_STA_JOINING) &&
	    state->bss.ssid_length == what->ssid_length && memcmp(state->bss.ssid, what->ssid, what->ssid_length) == 0;
	kfree_data(state, sizeof(*state));
	return same;
}

static void
ask_when_radio_is_up(__unused void *parameter, __unused wait_result_t result)
{
	struct hakios_sta_join what;
	unsigned waited = 0;
	int error;

	while (!hakios_sta_powered() && waited++ < WAIT_FOR_RADIO_SECONDS) {
		delay_for_interval(1000, NSEC_PER_MSEC);
	}
	lck_mtx_lock(&wifi_net_lock);
	what = given;
	memset_s(&given, sizeof(given), 0, sizeof(given));
	waiter_runs = false;
	lck_mtx_unlock(&wifi_net_lock);
	if (what.ssid_length == 0) {
		thread_terminate(current_thread());
		return;
	}
	if (!hakios_sta_powered()) {
		printf("wifi-net: the radio was not up after %u s: the kept network is not asked for\n", WAIT_FOR_RADIO_SECONDS);
	} else if (is_station_on(&what)) {
		printf("wifi-net: the kept network is the one the radio is on already\n");
	} else {
		error = hakios_sta_ask_join_standing(&what);
		printf("wifi-net: the kept network (a name of %u bytes, %s) asked for %u s after it was given: %s (%d)\n",
		    what.ssid_length, what.security == HAKIOS_STA_OPEN ? "open" : "WPA2 with a passphrase", waited,
		    error == 0 ? "the station joins it" : "REFUSED", error);
	}
	memset_s(&what, sizeof(what), 0, sizeof(what));
	thread_terminate(current_thread());
}

SYSCTL_DECL(_kern_hakios);

static int
sysctl_wifi_network SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char text[TEXT_MOST];
	struct hakios_sta_join what;
	size_t length = req->newlen;
	bool start;
	thread_t thread;
	int error;

	if (req->oldptr != USER_ADDR_NULL) {
		return EPERM;                   /* never read back */
	}
	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	if (length == 0 || length > sizeof(text)) {
		return EINVAL;
	}
	bzero(&what, sizeof(what));
	error = SYSCTL_IN(req, text, length);
	if (error == 0 && !parse_network(text, length, &what)) {
		error = EINVAL;
	}
	memset_s(text, sizeof(text), 0, sizeof(text));
	if (error != 0) {
		printf("wifi-net: what init26 gave is not a name and a passphrase (%zu bytes): %d\n", length, error);
		return error;
	}
	lck_mtx_lock(&wifi_net_lock);
	given = what;
	start = !waiter_runs;
	waiter_runs = true;
	lck_mtx_unlock(&wifi_net_lock);
	printf("wifi-net: a network was given (a name of %u bytes, %s): asked for once the radio is up\n", what.ssid_length,
	    what.security == HAKIOS_STA_OPEN ? "open" : "WPA2 with a passphrase");
	memset_s(&what, sizeof(what), 0, sizeof(what));
	if (start && kernel_thread_start(ask_when_radio_is_up, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else if (start) {
		lck_mtx_lock(&wifi_net_lock);
		waiter_runs = false;
		lck_mtx_unlock(&wifi_net_lock);
		return ENOMEM;
	}
	return 0;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, wifi_network, CTLTYPE_STRING | CTLFLAG_WR | CTLFLAG_LOCKED | CTLFLAG_MASKED, NULL, 0,
    sysctl_wifi_network, "A", "the network to join (name, newline, passphrase): written by init26, never read back");
