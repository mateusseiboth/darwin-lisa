/* Host stand-ins for the few kernel interfaces dlisa.ssc.c uses, so that its pure parts run in a host test. */
#ifndef KSHIM_H
#define KSHIM_H
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef __printflike
#define __printflike(a, b) __attribute__((format(printf, a, b)))
#endif
#ifndef __unused
#define __unused __attribute__((unused))
#endif
typedef int kern_return_t;
typedef int wait_result_t;
typedef void *thread_t;
typedef struct { int unused; } lck_mtx_t;
#define KERN_SUCCESS 0
#define THREAD_AWAKENED 0
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_SEC 1000000000ull
#define LCK_GRP_DECLARE(name, text) int name __attribute__((unused))
#define LCK_MTX_DECLARE(name, group) lck_mtx_t name
static inline void lck_mtx_lock(lck_mtx_t *m) { (void)m; }
static inline void lck_mtx_unlock(lck_mtx_t *m) { (void)m; }
#define os_atomic_inc(p, order) (++*(p))
#define os_atomic_load(p, order) (*(p))
static inline void delay_for_interval(uint64_t n, uint64_t unit) { (void)n; (void)unit; }
static inline uint64_t mach_absolute_time(void) { return 12345; }
static inline kern_return_t kernel_thread_start(void (*f)(void *, wait_result_t), void *p, thread_t *t) { (void)f; (void)p; (void)t; return 1; }
static inline void thread_deallocate(thread_t t) { (void)t; }
static inline thread_t current_thread(void) { return NULL; }
bool PE_parse_boot_argn(const char *name, void *value, int size);
#define SYSCTL_DECL(x) extern int x##_unused_decl
#define SYSCTL_OPAQUE(parent, nbr, name, flags, ptr, len, fmt, descr) extern int sysctl_##name##_unused
#define SYSCTL_STRING(parent, nbr, name, flags, ptr, len, descr) extern int sysctl_##name##_unused
#define CTLFLAG_RD 0
#define CTLFLAG_LOCKED 0
#endif
