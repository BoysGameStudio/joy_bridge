/* SPDX-License-Identifier: Apache-2.0 */
/*
 * journal — persistent flash-backed offline event log.
 * See include/journal/journal.h for the API and the design rationale.
 *
 * A curated, low-rate ring of runtime events kept on a flash partition so a
 * device's offline behaviour (battery powered, no serial console) can be
 * inspected after the fact. Implemented over Zephyr's Flash Circular Buffer
 * (FCB): power-fail-safe, self-rotating, CRC-checked entries.
 *
 * Events are never written to flash inline (writing flash can stall the XIP
 * instruction cache and disrupt tightly-timed work). journal_put() only enqueues
 * into a small RAM ring; a low-priority worker drains it to flash, and only while
 * the caller-supplied write-gate permits. journal_flush() drains synchronously
 * for a moment the caller guarantees is safe (e.g. just before deep sleep).
 */

#include "journal/journal.h"

#ifdef CONFIG_JOURNAL

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/fs/fcb.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/hwinfo.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(journal, CONFIG_JOURNAL_LOG_LEVEL);

/* Upper bound on sectors; the real count comes from flash_area_get_sectors().
 * 64 covers a 256 KiB partition at 4 KiB erase blocks. */
#define JOURNAL_MAX_SECTORS 64

/* Largest content length that still fits the slot after rounding the flash write
 * up to the 4-byte write block — guards the pad loop in fcb_write_line() against
 * a JOURNAL_LINE_MAX that is not a multiple of 4. */
#define JOURNAL_CONTENT_MAX ROUND_DOWN(CONFIG_JOURNAL_LINE_MAX, 4)

static struct fcb s_fcb;
static struct flash_sector s_sectors[JOURNAL_MAX_SECTORS];
static bool s_ready;
static uint8_t s_area_id;
static journal_write_gate_t s_gate;

/* RAM ring of pending lines, drained to flash by the worker. */
struct pend_line {
	uint16_t len;
	char     buf[CONFIG_JOURNAL_LINE_MAX];
};
static struct pend_line s_pend[CONFIG_JOURNAL_PEND_SLOTS];
static uint16_t s_pend_head;  /* next write slot */
static uint16_t s_pend_count; /* occupied slots */
static struct k_spinlock s_pend_lock;

/* ---- boot-time console dump --------------------------------------------- */
/* Written straight to the console UART (not via the deferred log backend), so a
 * multi-line replay is not dropped in bursts. Read-only: a dump runs before the
 * rest of the system comes up, so it cannot disrupt it. */

#if DT_HAS_CHOSEN(zephyr_console)
static void console_write(const char *s, size_t len)
{
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

static void console_puts(const char *s) { console_write(s, strlen(s)); }

/* By value: FCB_ENTRY_FA_DATA_OFF() needs a struct lvalue (it uses `loc.field`,
 * which misparses on `*ptr`). fcb_entry is small, so this is cheap. */
static void dump_entry(struct fcb_entry loc)
{
	char buf[CONFIG_JOURNAL_LINE_MAX];
	uint16_t len = MIN(loc.fe_data_len, (uint16_t)sizeof(buf));

	if (flash_area_read(s_fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc), buf, len) == 0) {
		console_write(buf, len);
		console_write("\n", 1);
	}
}

/* Replay the most recent CONFIG_JOURNAL_DUMP_LAST_N entries (oldest -> newest). */
static void dump_recent(void)
{
	struct fcb_entry loc;

	if (fcb_offset_last_n(&s_fcb, CONFIG_JOURNAL_DUMP_LAST_N, &loc) != 0) {
		return; /* empty ring */
	}
	do {
		dump_entry(loc);
	} while (fcb_getnext(&s_fcb, &loc) == 0);
}

/* ---- flash write (worker / sync flush only) ----------------------------- */

/* Append one already-formatted line to FCB. Performs a flash write, so callers
 * must ensure it is safe (write-gate open, or link down). */
static void fcb_write_line(char *line, size_t len)
{
	if (len > JOURNAL_CONTENT_MAX) {
		len = JOURNAL_CONTENT_MAX;
	}
	/* flash_area_write must be a multiple of the write-block size (commonly 4).
	 * FCB records the true length (len) for read-back and reserves the rounded-
	 * up slot, so the pad bytes are written but never read. len is capped above
	 * so wlen never exceeds the slot buffer. */
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
static int drain(int max_lines)
{
	int written = 0;

	while (s_ready && (max_lines == 0 || written < max_lines)) {
		struct pend_line line;
		k_spinlock_key_t key = k_spin_lock(&s_pend_lock);
		if (s_pend_count == 0) {
			k_spin_unlock(&s_pend_lock, key);
			break;
		}
		uint16_t tail = (uint16_t)((s_pend_head - s_pend_count +
					    CONFIG_JOURNAL_PEND_SLOTS) %
					   CONFIG_JOURNAL_PEND_SLOTS);
		line = s_pend[tail];
		s_pend_count--;
		k_spin_unlock(&s_pend_lock, key);

		fcb_write_line(line.buf, line.len);
		written++;
	}
	return written;
}

void journal_flush(void)
{
	drain(0); /* synchronous, unbounded — caller guarantees the write is safe */
}

/* ---- worker thread ------------------------------------------------------ */

static void flush_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		k_sleep(K_MSEC(CONFIG_JOURNAL_DRAIN_PERIOD_MS));
		if (!s_ready) {
			continue;
		}
		/* Only touch flash when the caller's write-gate permits (or always,
		 * if none was set). When the gate is closed the events simply wait;
		 * journal_flush() persists them at a known-safe moment. */
		if (s_gate == NULL || s_gate()) {
			(void)drain(CONFIG_JOURNAL_DRAIN_MAX_PER_RUN);
		}
	}
}
K_THREAD_DEFINE(journal_flush_tid, 1536, flush_thread_fn, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

/* ---- enqueue ------------------------------------------------------------ */

void journal_put(const char *fmt, ...)
{
	char buf[CONFIG_JOURNAL_LINE_MAX];

	/* Prefix the uptime (ms) so the replayed log carries relative timing. */
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

	/* Enqueue only — no flash here. Drop the oldest pending line if the ring is
	 * full rather than block the caller (which may be an ISR/callback). */
	k_spinlock_key_t key = k_spin_lock(&s_pend_lock);
	struct pend_line *slot = &s_pend[s_pend_head];
	slot->len = (uint16_t)len;
	memcpy(slot->buf, buf, len);
	s_pend_head = (uint16_t)((s_pend_head + 1) % CONFIG_JOURNAL_PEND_SLOTS);
	if (s_pend_count < CONFIG_JOURNAL_PEND_SLOTS) {
		s_pend_count++;
	}
	k_spin_unlock(&s_pend_lock, key);
}

/* ---- init --------------------------------------------------------------- */

int journal_init(uint8_t flash_area_id, journal_write_gate_t gate)
{
	s_area_id = flash_area_id;
	s_gate = gate;

	uint32_t cnt = JOURNAL_MAX_SECTORS;
	int rc = flash_area_get_sectors(s_area_id, &cnt, s_sectors);
	if (rc != 0) {
		LOG_ERR("flash_area_get_sectors failed: %d", rc);
		return rc;
	}

	s_fcb.f_magic = CONFIG_JOURNAL_FCB_MAGIC; /* must not be the erase value */
	s_fcb.f_version = 1;
	s_fcb.f_sector_cnt = (uint16_t)cnt;
	s_fcb.f_scratch_cnt = 0;
	s_fcb.f_sectors = s_sectors;

	rc = fcb_init(s_area_id, &s_fcb);
	if (rc != 0) {
		/* Unformatted or stale (e.g. leftover bytes from a prior use of this
		 * area): erase the whole area once and retry. */
		const struct flash_area *fa;
		if (flash_area_open(s_area_id, &fa) == 0) {
			(void)flash_area_erase(fa, 0, fa->fa_size);
			flash_area_close(fa);
		}
		rc = fcb_init(s_area_id, &s_fcb);
	}
	if (rc != 0) {
		LOG_ERR("fcb_init failed: %d", rc);
		return rc;
	}
	s_ready = true;

	/* Replay the stored log (most recent entries, oldest -> newest). Reads only
	 * — safe here, before the rest of the system is brought up. */
	console_puts("\n==== journal ====\n");
	dump_recent();
	console_puts("==== end journal ====\n");

	/* Boot marker with the reset cause (bitmask from <zephyr/drivers/hwinfo.h>).
	 * Enqueued (not written) so the early-boot path stays flash-free; the worker
	 * persists it once the write-gate opens. */
	uint32_t cause = 0;
	(void)hwinfo_get_reset_cause(&cause);
	journal_put("---- boot (reset_cause=0x%08x) ----", cause);

	return 0;
}

#endif /* CONFIG_JOURNAL */
