/*
 * Persistent event trace (see include/trace.h).
 *
 * A curated, low-rate ring of runtime events kept on a dedicated flash
 * partition so the device's offline behaviour (running on battery, no serial
 * console) can be inspected after the fact. Implemented over Zephyr's Flash
 * Circular Buffer (FCB): power-fail-safe, self-rotating, CRC-checked entries.
 *
 * WHY EVENTS ARE NOT WRITTEN INLINE (hard-won on hardware): writing the on-chip
 * SPI flash disables the XIP instruction cache for the duration of the write.
 * During the boot log-burst and the NS2 pairing handshake this corrupts the log
 * backend (the live console goes silent) and breaks pairing (security never
 * reaches L4). A write once the link is *settled* is tolerated. So jb_trace()
 * only enqueues into a small RAM ring; a low-priority worker drains it to flash
 * when the link is ready (ble_hid_ready()), and jb_trace_flush() drains it
 * synchronously just before deep sleep (link already down → safe). Verified on
 * hardware: with this split, console + pairing are unaffected and events still
 * land on flash.
 *
 * Isolation: this uses its OWN partition (log_partition). It never writes the
 * settings/NVS partition that holds the load-bearing NS2 bond.
 */

#include "trace.h"

#ifdef CONFIG_JOY_BRIDGE_PERSIST_LOG

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/fs/fcb.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/hwinfo.h>

#include "ble/ble_iface.h" /* ble_hid_ready() — flush gate */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jb_trace, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

/* DT node label of the partition carved in the board overlay. */
#define TRACE_AREA_ID     PARTITION_ID(log_partition)

/* 256 KiB partition / 4 KiB erase block = 64 sectors. Sized generously; the
 * actual count comes from flash_area_get_sectors() at init. */
#define TRACE_MAX_SECTORS 64

/* Max formatted line, including the uptime prefix. A multiple of the 4-byte
 * flash write block so a full line needs no extra padding. */
#define TRACE_LINE_MAX    96

/* Boot-replay is bounded to the most recent N entries — the ring can hold
 * thousands of lines before it wraps, and the whole dump goes out byte-by-byte
 * over the console UART. ~100 lines ≈ the last ~20 sessions. */
#define TRACE_DUMP_LAST_N 100

/* Pending-event RAM ring drained to flash by the worker. Holds the boot marker
 * plus the connect/pair burst until the link settles and the first drain runs. */
#define PEND_SLOTS        24

/* Background drain cadence + per-cycle cap (keeps each flash burst small so it
 * can't perturb the active link). */
#define DRAIN_PERIOD_MS   2000
#define DRAIN_MAX_PER_RUN 4

static struct fcb s_fcb;
static struct flash_sector s_sectors[TRACE_MAX_SECTORS];
static bool s_ready;

/* RAM ring of pending lines. */
struct pend_line {
    uint8_t len;
    char    buf[TRACE_LINE_MAX];
};
static struct pend_line s_pend[PEND_SLOTS];
static uint8_t s_pend_head;  /* next write slot */
static uint8_t s_pend_count; /* occupied slots */
static struct k_spinlock s_pend_lock;

/* ---- boot-time console dump --------------------------------------------- */
/* Written straight to the console UART (not via the deferred log backend),
 * because the replay can be many lines and the log backend would drop them in
 * bursts. Read-only: a dump runs before BLE comes up, so it cannot disrupt it. */

#if DT_HAS_CHOSEN(zephyr_console)
static void console_write(const char *s, size_t len) {
    const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

    if (!device_is_ready(con)) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            uart_poll_out(con, '\r');
        }
        uart_poll_out(con, s[i]);
    }
}
#else
static void console_write(const char *s, size_t len) { ARG_UNUSED(s); ARG_UNUSED(len); }
#endif

/* By value: FCB_ENTRY_FA_DATA_OFF() needs a struct lvalue (it uses `loc.field`,
 * which misparses on `*ptr`). fcb_entry is small, so this is cheap. */
static void dump_entry(struct fcb_entry loc) {
    char buf[TRACE_LINE_MAX];
    uint16_t len = MIN(loc.fe_data_len, (uint16_t)sizeof(buf));

    if (flash_area_read(s_fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc), buf, len) == 0) {
        console_write(buf, len);
        console_write("\n", 1);
    }
}

/* Replay the most recent TRACE_DUMP_LAST_N entries (oldest -> newest). */
static void dump_recent(void) {
    struct fcb_entry loc;

    if (fcb_offset_last_n(&s_fcb, TRACE_DUMP_LAST_N, &loc) != 0) {
        return; /* empty ring */
    }
    do {
        dump_entry(loc);
    } while (fcb_getnext(&s_fcb, &loc) == 0);
}

/* ---- flash write (worker / sync flush only) ----------------------------- */

/* Append one already-formatted line to FCB. Performs a flash write, so callers
 * must ensure it is safe (link settled or down). */
static void fcb_write_line(char *line, size_t len) {
    if (len > TRACE_LINE_MAX) {
        len = TRACE_LINE_MAX;
    }
    /* flash_area_write must be a multiple of the write-block size (4 on the
     * S3). FCB records the true length (len) for read-back and reserves the
     * rounded-up slot, so the pad bytes are written but never read. */
    size_t wlen = ROUND_UP(len, 4);
    for (size_t i = len; i < wlen; i++) {
        line[i] = ' ';
    }

    struct fcb_entry loc;
    int rc = fcb_append(&s_fcb, (uint16_t)len, &loc);
    if (rc == -ENOSPC) {
        /* Ring full: drop the oldest sector and retry once. */
        if (fcb_rotate(&s_fcb) == 0) {
            rc = fcb_append(&s_fcb, (uint16_t)len, &loc);
        }
    }
    if (rc == 0) {
        rc = flash_area_write(s_fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc), line, wlen);
        if (rc == 0) {
            (void)fcb_append_finish(&s_fcb, &loc);
        }
    }
}

/* Pop up to max_lines from the RAM ring and write them to flash. max_lines == 0
 * means drain everything. Returns the number written. */
static int drain(int max_lines) {
    int written = 0;

    while (s_ready && (max_lines == 0 || written < max_lines)) {
        struct pend_line line;
        k_spinlock_key_t key = k_spin_lock(&s_pend_lock);
        if (s_pend_count == 0) {
            k_spin_unlock(&s_pend_lock, key);
            break;
        }
        uint8_t tail = (uint8_t)((s_pend_head - s_pend_count + PEND_SLOTS) % PEND_SLOTS);
        line = s_pend[tail];
        s_pend_count--;
        k_spin_unlock(&s_pend_lock, key);

        fcb_write_line(line.buf, line.len);
        written++;
    }
    return written;
}

void jb_trace_flush(void) {
    drain(0); /* synchronous, unbounded — caller guarantees the link is down */
}

/* ---- worker thread ------------------------------------------------------ */

static void flush_thread_fn(void *a, void *b, void *c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    for (;;) {
        k_sleep(K_MSEC(DRAIN_PERIOD_MS));
        if (!s_ready) {
            continue;
        }
        /* Only touch flash while the link is settled (post-pairing, HID flowing)
         * — never during the boot burst or the pairing handshake. When idle /
         * advertising the events simply wait; deep sleep flushes them. */
        if (ble_hid_ready()) {
            (void)drain(DRAIN_MAX_PER_RUN);
        }
    }
}
K_THREAD_DEFINE(jb_trace_flush_tid, 1536, flush_thread_fn, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

/* ---- enqueue ------------------------------------------------------------ */

void jb_trace(const char *fmt, ...) {
    char buf[TRACE_LINE_MAX];

    /* Prefix the uptime (ms) so the replayed trace carries relative timing. */
    int p = snprintf(buf, sizeof(buf), "%llu ", (unsigned long long)k_uptime_get());
    if (p < 0) {
        return;
    }
    size_t off = MIN((size_t)p, sizeof(buf) - 1);

    va_list ap;
    va_start(ap, fmt);
    int b = vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    va_end(ap);
    if (b < 0) {
        return;
    }
    size_t len = MIN(off + (size_t)b, sizeof(buf) - 1);

    /* Enqueue only — no flash here (see file header). Drop the oldest pending
     * line if the ring is full rather than block a BLE/USB callback. */
    k_spinlock_key_t key = k_spin_lock(&s_pend_lock);
    struct pend_line *slot = &s_pend[s_pend_head];
    slot->len = (uint8_t)len;
    memcpy(slot->buf, buf, len);
    s_pend_head = (uint8_t)((s_pend_head + 1) % PEND_SLOTS);
    if (s_pend_count < PEND_SLOTS) {
        s_pend_count++;
    }
    k_spin_unlock(&s_pend_lock, key);
}

/* ---- init --------------------------------------------------------------- */

void jb_trace_init(void) {
    uint32_t cnt = TRACE_MAX_SECTORS;
    int rc = flash_area_get_sectors(TRACE_AREA_ID, &cnt, s_sectors);
    if (rc != 0) {
        ESP_LOGE("trace", "flash_area_get_sectors failed: %d", rc);
        return;
    }

    s_fcb.f_magic = 0x6a626c67; /* "jblg" — must not be the erase value */
    s_fcb.f_version = 1;
    s_fcb.f_sector_cnt = (uint16_t)cnt;
    s_fcb.f_scratch_cnt = 0;
    s_fcb.f_sectors = s_sectors;

    rc = fcb_init(TRACE_AREA_ID, &s_fcb);
    if (rc != 0) {
        /* Unformatted or stale (e.g. leftover NVS bytes from before this
         * partition existed): erase the whole area once and retry. */
        const struct flash_area *fa;
        if (flash_area_open(TRACE_AREA_ID, &fa) == 0) {
            (void)flash_area_erase(fa, 0, fa->fa_size);
            flash_area_close(fa);
        }
        rc = fcb_init(TRACE_AREA_ID, &s_fcb);
    }
    if (rc != 0) {
        ESP_LOGE("trace", "fcb_init failed: %d", rc);
        return;
    }
    s_ready = true;

    /* Replay the stored trace (most recent entries, oldest -> newest). Reads
     * only — safe here, before BLE is brought up. */
    console_write("\n==== persisted trace ====\n", 27);
    dump_recent();
    console_write("==== end trace ====\n", 20);

    /* Boot marker with the reset cause (bitmask from <zephyr/drivers/hwinfo.h>):
     * RESET_BROWNOUT (0x04) flags a battery sag while offline, RESET_POR (0x08) a
     * clean power-on, RESET_LOW_POWER_WAKE (0x80) a wake from the NS2-off deep
     * sleep, RESET_PIN (0x01) the EN/RESET button. Enqueued (not written) so the
     * early-boot path stays flash-free; the worker persists it once settled. */
    uint32_t cause = 0;
    (void)hwinfo_get_reset_cause(&cause);
    jb_trace("---- boot (reset_cause=0x%08x) ----", cause);
}

#endif /* CONFIG_JOY_BRIDGE_PERSIST_LOG */
