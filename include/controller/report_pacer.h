#ifndef _CONTROLLER_REPORT_PACER_H_
#define _CONTROLLER_REPORT_PACER_H_

/*
 * HID report send pacer — pure timing logic, no Zephyr dependencies so it can
 * be unit-tested on the host (see tests/report_pacer/).
 *
 * The report thread calls report_pacer_delay_ms() before every send and
 * report_pacer_on_sent() after. Two cooperating mechanisms:
 *
 *  - Interval tracking: the caller passes the negotiated BLE connection
 *    interval (clamped to [5, 30] ms by report_pacer_clamp_interval), so the
 *    send grid speeds up on a fast link and backs off when the console
 *    power-saves, instead of overunning the BLE TX pool at a fixed rate.
 *  - Fresh-input burst: committed input sends immediately while tokens last
 *    (refilled at one per interval, capped at burst_max), so sparse
 *    change-driven input never waits out the keepalive grid; dense streams
 *    degrade to the interval cap once the burst is spent.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REPORT_PACER_MIN_INTERVAL_MS 5u
#define REPORT_PACER_MAX_INTERVAL_MS 30u

typedef struct {
    int64_t last_send_ms;   /* uptime of the last send; 0 = never sent */
    int64_t last_refill_ms; /* uptime base of the last token refill; 0 = unset */
    uint8_t tokens;         /* remaining fresh-input burst budget */
    uint8_t burst_max;      /* token ceiling */
} report_pacer_t;

/* Initialise with a full burst budget. */
void report_pacer_init(report_pacer_t *pacer, uint8_t burst_max);

/* Link lost: forget the send grid and re-arm the full burst. */
void report_pacer_reset(report_pacer_t *pacer);

/* Clamp a negotiated interval (ms) into the supported send-grid range. */
uint16_t report_pacer_clamp_interval(uint16_t interval_ms);

/* Milliseconds to sleep BEFORE sending (0 = send immediately). Refills the
 * token bucket lazily from elapsed time. fresh = a new input report was
 * committed since the last send. interval_ms must already be clamped. */
uint32_t report_pacer_delay_ms(report_pacer_t *pacer, bool fresh,
                               int64_t now_ms, uint16_t interval_ms);

/* Record a completed send; consumes one token when a fresh report was sent
 * through the burst path. */
void report_pacer_on_sent(report_pacer_t *pacer, bool fresh, int64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* _CONTROLLER_REPORT_PACER_H_ */
