/*
 * HID report send pacer — pure timing logic (see include/controller/report_pacer.h
 * for the two cooperating mechanisms and the API contract).
 */

#include "controller/report_pacer.h"

void report_pacer_init(report_pacer_t *pacer, uint8_t burst_max)
{
    if (pacer == NULL) {
        return;
    }
    pacer->last_send_ms = 0;
    pacer->last_refill_ms = 0;
    pacer->tokens = burst_max;
    pacer->burst_max = burst_max;
}

void report_pacer_reset(report_pacer_t *pacer)
{
    if (pacer == NULL) {
        return;
    }
    pacer->last_send_ms = 0;
    pacer->last_refill_ms = 0;
    pacer->tokens = pacer->burst_max;
}

uint16_t report_pacer_clamp_interval(uint16_t interval_ms)
{
    if (interval_ms < REPORT_PACER_MIN_INTERVAL_MS) {
        return REPORT_PACER_MIN_INTERVAL_MS;
    }
    if (interval_ms > REPORT_PACER_MAX_INTERVAL_MS) {
        return REPORT_PACER_MAX_INTERVAL_MS;
    }
    return interval_ms;
}

/* Lazy refill: one token per full elapsed interval, capped at burst_max. The
 * remainder carries over (last_refill advances by whole chunks only). */
static void refill(report_pacer_t *pacer, int64_t now_ms, uint16_t interval_ms)
{
    if (pacer->last_refill_ms == 0) {
        pacer->last_refill_ms = now_ms;
        return;
    }
    const int64_t elapsed = now_ms - pacer->last_refill_ms;
    if (elapsed < (int64_t)interval_ms) {
        return;
    }
    const uint32_t chunks = (uint32_t)(elapsed / (int64_t)interval_ms);
    const uint32_t sum = (uint32_t)pacer->tokens + chunks;
    pacer->tokens = (sum > pacer->burst_max) ? pacer->burst_max : (uint8_t)sum;
    pacer->last_refill_ms += (int64_t)chunks * (int64_t)interval_ms;
}

uint32_t report_pacer_delay_ms(report_pacer_t *pacer, bool fresh,
                               int64_t now_ms, uint16_t interval_ms)
{
    if (pacer == NULL || interval_ms == 0u) {
        return 0u;
    }
    refill(pacer, now_ms, interval_ms);

    if (fresh && pacer->tokens > 0u) {
        return 0u; /* burst path: committed input goes out immediately */
    }
    if (pacer->last_send_ms == 0) {
        return 0u; /* never sent (or link just restored): no grid to keep */
    }
    const int64_t due = pacer->last_send_ms + (int64_t)interval_ms;
    return (now_ms >= due) ? 0u : (uint32_t)(due - now_ms);
}

void report_pacer_on_sent(report_pacer_t *pacer, bool fresh, int64_t now_ms)
{
    if (pacer == NULL) {
        return;
    }
    pacer->last_send_ms = now_ms;
    if (fresh && pacer->tokens > 0u) {
        pacer->tokens--;
    }
}
