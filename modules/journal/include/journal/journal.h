/* SPDX-License-Identifier: Apache-2.0 */
#ifndef JOURNAL_JOURNAL_H_
#define JOURNAL_JOURNAL_H_

/*
 * journal — a reusable Zephyr persistent event log with an Unreal-Engine-style
 * logging macro.
 *
 * The JOURNAL() macro is modelled on UE_LOG(Category, Verbosity, Format, ...):
 *
 *     JOURNAL(Category, Verbosity, Target, fmt, ...)
 *     JOURNAL_IF(cond, Category, Verbosity, Target, fmt, ...)   // = UE_CLOG
 *
 *   Category  — a bare UPPERCASE token naming the event class / concern (e.g.
 *               CONN, SEC, PWR, PAD), orthogonal to the Zephyr LOG module (which
 *               already names the file). Stringized and prefixed as "[CATEGORY]"
 *               to every line, so it adds information instead of echoing the
 *               module name.
 *   Verbosity — UE's ELogVerbosity names, mapped to the Zephyr log level used
 *               for the console sink:
 *                 Fatal       -> LOG_ERR  (then flush + k_panic())
 *                 Error       -> LOG_ERR
 *                 Warning     -> LOG_WRN
 *                 Display     -> LOG_INF
 *                 Log         -> LOG_INF
 *                 Verbose     -> LOG_DBG
 *                 VeryVerbose -> LOG_DBG
 *   Target    — where the line goes (UE conflates this into verbosity; here it
 *               is explicit):
 *                 Console — live Zephyr console log only
 *                 Flash   — persistent journal (flash ring) only
 *                 All     — console + flash
 *
 * Fatal always goes to both sinks, flushes synchronously, then panics —
 * regardless of Target (UE: Fatal always prints, then crashes).
 *
 * The persistent half mirrors UE's "log file": curated lines are kept in a
 * power-fail-safe flash ring (Zephyr FCB) so a headless / battery device's event
 * history can be replayed after the fact (next boot, or pulled from flash).
 *
 * Events are NOT written to flash inline: the Flash/All targets only enqueue into
 * a small RAM ring; a low-priority worker drains it to flash later, and only
 * while the optional write-gate (see journal_init) permits — keeping flash writes
 * out of timing-sensitive windows (radio handshakes, boot bursts) where stalling
 * the XIP instruction cache would cause harm. journal_flush() drains
 * synchronously for the one moment the caller knows is safe (e.g. before deep
 * sleep, link down).
 *
 * The backing partition is chosen by the caller: pass its flash-area id to
 * journal_init() (FIXED_PARTITION_ID(), or PARTITION_ID() for a mapped
 * partition). The module hardcodes no devicetree label.
 *
 * Gated by CONFIG_JOURNAL: when disabled, the Flash/All persistence vanishes and
 * JOURNAL() still logs to the console at the chosen level (the flash half
 * compiles to no-ops).
 *
 * NOTE: fmt must be a string literal (it is concatenated with the "[Category]"
 * prefix), and the format arguments are evaluated twice for the All target —
 * keep them side-effect-free.
 *
 * Usage (in a .c that already does LOG_MODULE_REGISTER):
 *     #include <journal/journal.h>
 *     journal_init(FIXED_PARTITION_ID(my_log), my_write_gate);
 *     JOURNAL(CONN, Display, All,     "connect itvl=%u", itvl);
 *     JOURNAL(PAD,  Warning, Console, "stall ep=0x%02x", ep);
 *     JOURNAL(PWR,  Display, Flash,   "deep sleep");
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>          /* k_panic() for the Fatal verbosity */
#include <zephyr/logging/log.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional flash-write gate. While set, the background worker drains the RAM ring
 * to flash only when this returns true; NULL/unset means "always safe to drain".
 * Lets a project defer flash writes during sensitive phases.
 */
typedef bool (*journal_write_gate_t)(void);

#ifdef CONFIG_JOURNAL

/*
 * Mount the FCB ring on flash area `flash_area_id`, replay the most recent stored
 * events to the console, and append a boot marker (with the reset cause). `gate`
 * may be NULL. Call once, early in main(). Returns 0 on success, <0 on error.
 */
int journal_init(uint8_t flash_area_id, journal_write_gate_t gate);

/*
 * Synchronously drain all pending events to flash. Call ONLY when a flash write
 * is safe (e.g. before deep sleep, link down).
 */
void journal_flush(void);

/*
 * Internal — NOT part of the public API. The JOURNAL() macro's flash sink:
 * enqueues one preformatted line to the RAM ring (drained to flash later by the
 * worker; no console output, no flash I/O in this call). Exposed only because the
 * macro expands in caller translation units — applications use
 * JOURNAL() / JOURNAL_IF(), never this directly.
 */
__printf_like(1, 2) void z_journal_emit(const char *fmt, ...);

#else /* !CONFIG_JOURNAL */

static inline int journal_init(uint8_t flash_area_id, journal_write_gate_t gate)
{
	ARG_UNUSED(flash_area_id);
	ARG_UNUSED(gate);
	return 0;
}
static inline void journal_flush(void) {}
static inline __printf_like(1, 2) void z_journal_emit(const char *fmt, ...)
{
	ARG_UNUSED(fmt);
}

#endif /* CONFIG_JOURNAL */

/* ---- JOURNAL() macro (UE_LOG-style) ------------------------------------- *
 * The dispatch is config-independent: the Flash/All paths go through
 * z_journal_emit(), which is a no-op stub when CONFIG_JOURNAL=n, so the same macro
 * degrades to console-only with no #ifdef at the call site.
 *
 * Two-stage token paste: first on Verbosity (to pick the console level + the
 * flash severity tag, and to special-case Fatal), then on Target (to pick the
 * sink). The Zephyr LOG_xxx macro is passed by name and invoked in the target
 * stage. */

/* Target stage: `logmac` is the console LOG_xxx; `tag` is the flash severity
 * prefix. Console ignores `tag`; Flash ignores `logmac`. */
#define _JRN_T_Console(logmac, tag, cat, fmt, ...)                              \
	logmac("[" #cat "] " fmt, ##__VA_ARGS__)
#define _JRN_T_Flash(logmac, tag, cat, fmt, ...)                                \
	z_journal_emit("[" #cat "] " tag fmt, ##__VA_ARGS__)
#define _JRN_T_All(logmac, tag, cat, fmt, ...)                                  \
	do {                                                                    \
		logmac("[" #cat "] " fmt, ##__VA_ARGS__);                       \
		z_journal_emit("[" #cat "] " tag fmt, ##__VA_ARGS__);              \
	} while (0)

/* Verbosity stage: forward to the chosen target with (console level, flash tag).
 * Fatal ignores Target: both sinks + synchronous flush + panic. */
#define _JRN_V_Fatal(cat, target, fmt, ...)                                     \
	do {                                                                    \
		LOG_ERR("[" #cat "] FATAL " fmt, ##__VA_ARGS__);                \
		z_journal_emit("[" #cat "] FATAL " fmt, ##__VA_ARGS__);            \
		journal_flush();                                                \
		k_panic();                                                      \
	} while (0)
#define _JRN_V_Error(cat, target, fmt, ...)                                     \
	_JRN_T_##target(LOG_ERR, "ERR ", cat, fmt, ##__VA_ARGS__)
#define _JRN_V_Warning(cat, target, fmt, ...)                                   \
	_JRN_T_##target(LOG_WRN, "WRN ", cat, fmt, ##__VA_ARGS__)
#define _JRN_V_Display(cat, target, fmt, ...)                                   \
	_JRN_T_##target(LOG_INF, "", cat, fmt, ##__VA_ARGS__)
#define _JRN_V_Log(cat, target, fmt, ...)                                       \
	_JRN_T_##target(LOG_INF, "", cat, fmt, ##__VA_ARGS__)
#define _JRN_V_Verbose(cat, target, fmt, ...)                                   \
	_JRN_T_##target(LOG_DBG, "", cat, fmt, ##__VA_ARGS__)
#define _JRN_V_VeryVerbose(cat, target, fmt, ...)                               \
	_JRN_T_##target(LOG_DBG, "", cat, fmt, ##__VA_ARGS__)

/* Public front door. */
#define JOURNAL(cat, verb, target, fmt, ...)                                    \
	_JRN_V_##verb(cat, target, fmt, ##__VA_ARGS__)

/* Conditional variant (UE_CLOG). */
#define JOURNAL_IF(cond, cat, verb, target, fmt, ...)                           \
	do {                                                                    \
		if (cond) {                                                     \
			JOURNAL(cat, verb, target, fmt, ##__VA_ARGS__);         \
		}                                                               \
	} while (0)

#ifdef __cplusplus
}
#endif

#endif /* JOURNAL_JOURNAL_H_ */
