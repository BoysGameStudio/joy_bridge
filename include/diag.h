/*
 * DIAG — config-gated hexdump capture of the NS2 <-> controller ATT traffic.
 *
 * Enable with CONFIG_JOY_BRIDGE_DIAG_HEXDUMP=y (and, for the high-rate HID
 * input reports, CONFIG_JOY_BRIDGE_DIAG_HEXDUMP_HID=y). Every captured PDU is
 * emitted at INFO level tagged "[DIAG] ...", so a serial capture can be grepped
 * for "[DIAG]" and the raw bytes lifted out for offline analysis (e.g. feed to
 * Wireshark via text2pcap). Disabled builds compile these to nothing.
 *
 * Include AFTER LOG_MODULE_REGISTER()/app_log.h in the .c file so the LOG_*
 * macros bind to that translation unit's log module.
 *
 * The tag passed to LOG_DIAG_DUMP must be a string literal: it is concatenated
 * with "[DIAG] " at compile time, which also keeps the hexdump label pointer in
 * static storage (deferred logging stores the label by pointer, not by copy).
 */
#ifndef _DIAG_H_
#define _DIAG_H_

#include <zephyr/logging/log.h>

#if defined(CONFIG_JOY_BRIDGE_DIAG_HEXDUMP)
#define LOG_DIAG(...)              LOG_INF(__VA_ARGS__)
#define LOG_DIAG_DUMP(tag, d, l)   LOG_HEXDUMP_INF((d), (l), "[DIAG] " tag)
#else
#define LOG_DIAG(...)              ((void)0)
#define LOG_DIAG_DUMP(tag, d, l)   ((void)0)
#endif

#if defined(CONFIG_JOY_BRIDGE_DIAG_HEXDUMP_HID)
#define LOG_DIAG_DUMP_HID(tag, d, l)  LOG_HEXDUMP_INF((d), (l), "[DIAG] " tag)
#else
#define LOG_DIAG_DUMP_HID(tag, d, l)  ((void)0)
#endif

#endif /* _DIAG_H_ */
