/*
 * ztest suite for the pure HID report send pacer
 * (src/controller/report_pacer.c). qemu_x86 / native_sim only; the pacer
 * under test has zero Zephyr dependencies and is compiled standalone.
 */

#include <zephyr/ztest.h>

#include "controller/report_pacer.h"

#define BURST 3u
#define ITVL 15u

/* (1) clamp: below/inside/above the [5, 30] ms window. */
ZTEST(report_pacer, test_clamp_bounds)
{
    zassert_equal(report_pacer_clamp_interval(0u), 5u, "0 not clamped up");
    zassert_equal(report_pacer_clamp_interval(4u), 5u, "4 not clamped up");
    zassert_equal(report_pacer_clamp_interval(5u), 5u, "5 changed");
    zassert_equal(report_pacer_clamp_interval(15u), 15u, "15 changed");
    zassert_equal(report_pacer_clamp_interval(30u), 30u, "30 changed");
    zassert_equal(report_pacer_clamp_interval(31u), 30u, "31 not clamped down");
    zassert_equal(report_pacer_clamp_interval(1000u), 30u, "1000 not clamped down");
}

/* (2) fresh input with tokens sends immediately; each burst send eats a token. */
ZTEST(report_pacer, test_fresh_burst_immediate)
{
    report_pacer_t p;
    report_pacer_init(&p, BURST);

    for (uint8_t i = 0u; i < BURST; i++) {
        zassert_equal(report_pacer_delay_ms(&p, true, 1000, ITVL), 0u,
                      "burst send %u delayed", i);
        report_pacer_on_sent(&p, true, 1000);
        zassert_equal(p.tokens, BURST - 1u - i, "token not consumed");
    }
}

/* (3) burst exhausted -> fresh input falls back to the interval grid. */
ZTEST(report_pacer, test_burst_exhaustion_uses_grid)
{
    report_pacer_t p;
    report_pacer_init(&p, BURST);

    for (uint8_t i = 0u; i < BURST; i++) {
        zassert_equal(report_pacer_delay_ms(&p, true, 1000, ITVL), 0u,
                      "burst send %u delayed", i);
        report_pacer_on_sent(&p, true, 1000);
    }
    zassert_equal(p.tokens, 0u, "tokens left");

    /* Same instant: a full interval is owed since last_send. */
    zassert_equal(report_pacer_delay_ms(&p, true, 1000, ITVL), ITVL,
                  "exhausted fresh send not grid-capped");
    /* 6 ms later: the remainder is owed. */
    zassert_equal(report_pacer_delay_ms(&p, true, 1006, ITVL), ITVL - 6u,
                  "remainder wrong");
}

/* (4) tokens refill one per interval and cap at burst_max. */
ZTEST(report_pacer, test_refill_rate_and_cap)
{
    report_pacer_t p;
    report_pacer_init(&p, BURST);

    /* Spend the whole burst. */
    for (uint8_t i = 0u; i < BURST; i++) {
        (void)report_pacer_delay_ms(&p, true, 1000, ITVL);
        report_pacer_on_sent(&p, true, 1000);
    }
    zassert_equal(p.tokens, 0u, "tokens left after burst");

    /* One interval later: exactly one token back -> immediate again. */
    zassert_equal(report_pacer_delay_ms(&p, true, 1000 + ITVL, ITVL), 0u,
                  "no token after one interval");

    /* Spend it, then idle far longer than burst_max intervals: the bucket
     * must cap at burst_max, not accumulate without bound. */
    report_pacer_on_sent(&p, true, 1000 + ITVL);
    zassert_equal(report_pacer_delay_ms(&p, true, 1000 + 100u * ITVL, ITVL), 0u,
                  "refill failed after long idle");
    zassert_equal(p.tokens, BURST, "bucket exceeded burst_max");
}

/* (5) keepalive (non-fresh) sends follow the grid and never spend tokens. */
ZTEST(report_pacer, test_keepalive_grid_no_tokens)
{
    report_pacer_t p;
    report_pacer_init(&p, BURST);

    report_pacer_on_sent(&p, false, 1000);
    zassert_equal(report_pacer_delay_ms(&p, false, 1010, ITVL), 5u,
                  "keepalive not held to grid");
    zassert_equal(report_pacer_delay_ms(&p, false, 1015, ITVL), 0u,
                  "keepalive not released at due");
    report_pacer_on_sent(&p, false, 1015);
    zassert_equal(p.tokens, BURST, "keepalive consumed a token");
}

/* (6) never-sent pacer (last_send unset) sends immediately, fresh or not. */
ZTEST(report_pacer, test_first_send_immediate)
{
    report_pacer_t p;
    report_pacer_init(&p, BURST);

    zassert_equal(report_pacer_delay_ms(&p, false, 5000, ITVL), 0u,
                  "first keepalive delayed");
    zassert_equal(report_pacer_delay_ms(&p, true, 5000, ITVL), 0u,
                  "first fresh send delayed");
}

/* (7) reset (link loss) re-arms the burst and forgets the send grid. */
ZTEST(report_pacer, test_reset_rearms)
{
    report_pacer_t p;
    report_pacer_init(&p, BURST);

    for (uint8_t i = 0u; i < BURST; i++) {
        (void)report_pacer_delay_ms(&p, true, 2000, ITVL);
        report_pacer_on_sent(&p, true, 2000);
    }
    zassert_equal(report_pacer_delay_ms(&p, true, 2000, ITVL), ITVL,
                  "grid not engaged before reset");

    report_pacer_reset(&p);
    zassert_equal(p.tokens, BURST, "burst not re-armed");
    zassert_equal(report_pacer_delay_ms(&p, true, 2000, ITVL), 0u,
                  "fresh send after reset delayed");
    /* The forgotten grid must not resurrect: non-fresh also immediate. */
    zassert_equal(report_pacer_delay_ms(&p, false, 2001, ITVL), 0u,
                  "stale send grid survived reset");
}

ZTEST_SUITE(report_pacer, NULL, NULL, NULL, NULL, NULL);
