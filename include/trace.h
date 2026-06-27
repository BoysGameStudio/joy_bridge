#ifndef _TRACE_H_
#define _TRACE_H_

/*
 * Persistent event trace — a curated, low-rate log of significant runtime
 * events written to a dedicated flash partition (log_partition) so the device's
 * behaviour while running *offline* (on battery, no serial console attached) can
 * be reviewed later.
 *
 * On boot the previously stored trace is dumped to the console; new events are
 * appended durably (each append commits to flash, so an abrupt power loss — e.g.
 * a flat battery — loses at most the line in flight). Backed by Zephyr's Flash
 * Circular Buffer over its OWN partition; it never touches the settings/NVS
 * partition that holds the load-bearing NS2 bond. See src/app/trace.c.
 *
 * Gated by CONFIG_JOY_BRIDGE_PERSIST_LOG; compiles to nothing when disabled.
 */

#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_JOY_BRIDGE_PERSIST_LOG

/* Mount the flash ring, replay any stored events to the console, and append a
 * boot marker (with the reset cause). Call once, early in main(). */
void jb_trace_init(void);

/* Record one curated event. The line is enqueued to a small RAM ring and
 * written to flash later by a low-priority worker (only once the BLE link is
 * settled) — NEVER inline, because writing the on-chip flash stalls the XIP
 * cache and disrupts the BLE controller and the boot/pairing path. The caller
 * keeps its own LOG_* line for the live console. Low-rate use only; safe from
 * any thread context (BLE/USB callbacks included). */
__printf_like(1, 2) void jb_trace(const char *fmt, ...);

/* Synchronously flush all pending events to flash. Call ONLY when the BLE link
 * is down (e.g. just before deep sleep), where a flash write is safe. */
void jb_trace_flush(void);

#else /* !CONFIG_JOY_BRIDGE_PERSIST_LOG */

static inline void jb_trace_init(void) {}
static inline __printf_like(1, 2) void jb_trace(const char *fmt, ...) { ARG_UNUSED(fmt); }
static inline void jb_trace_flush(void) {}

#endif /* CONFIG_JOY_BRIDGE_PERSIST_LOG */

#ifdef __cplusplus
}
#endif

#endif /* _TRACE_H_ */
