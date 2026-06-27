journal — persistent flash-backed offline event log (Zephyr module)
==================================================================

A small, reusable Zephyr module that mirrors curated log lines to a power-fail-safe
flash ring (Zephyr Flash Circular Buffer) so a headless / battery-powered device's
event history can be reviewed after the fact — replayed to the console on the next
boot, or pulled straight from the flash partition.

The ``JOURNAL()`` macro is modelled on Unreal Engine's
``UE_LOG(Category, Verbosity, ...)``: one call, one format string, routed to the
live console log and/or the flash ring — so there is no duplicated wording to keep
in sync.

Why a flash ring, written off the hot path
------------------------------------------

Writing on-chip flash can stall the XIP instruction cache for the duration of the
write, which disrupts tightly-timed work (radio handshakes, the boot log burst).
So ``journal_put()`` never touches flash: it only enqueues into a small RAM ring.
A low-priority worker drains the ring to flash later, and **only while the
caller-supplied write-gate permits** — letting a project keep flash writes out of
its sensitive windows. ``journal_flush()`` drains synchronously for the one moment
the caller knows is safe (e.g. just before deep sleep, link down).

API
---

.. code-block:: c

   #include <journal/journal.h>

   int  journal_init(uint8_t flash_area_id, journal_write_gate_t gate);
   void journal_flush(void);
   void journal_put(const char *fmt, ...);   /* low-level persist-only primitive */

   /* UE_LOG-style front door: */
   JOURNAL(Category, Verbosity, Target, fmt, ...);
   JOURNAL_IF(cond, Category, Verbosity, Target, fmt, ...);   /* = UE_CLOG */

   /* Category  — bare subsystem token, prefixed as "[Category]".
    * Verbosity — UE names -> console log level:
    *   Fatal->LOG_ERR(+flush+k_panic)  Error->LOG_ERR  Warning->LOG_WRN
    *   Display/Log->LOG_INF  Verbose/VeryVerbose->LOG_DBG
    * Target    — Console (live log only) | Flash (journal only) | All (both). */

``flash_area_id`` is resolved by the caller with ``FIXED_PARTITION_ID(label)`` —
or ``PARTITION_ID(label)`` for a ``zephyr,mapped-partition``. The module hardcodes
no devicetree label. ``gate`` may be ``NULL`` (always drain).

Using it in a project
---------------------

1. Add the module before ``find_package(Zephyr ...)`` in the app ``CMakeLists.txt``::

       list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/journal)

   (or copy / git-submodule the module elsewhere and point the path at it; or add a
   west manifest entry).

2. Enable it and provide a flash partition::

       CONFIG_JOURNAL=y

   Carve a dedicated partition in your board overlay (its own area, separate from
   any settings/NVS partition you care about).

3. Initialise early in ``main()`` and record events::

       journal_init(FIXED_PARTITION_ID(my_log), my_write_gate);
       JOURNAL(Ble, Display, All,     "link up itvl=%u", itvl);
       JOURNAL(Usb, Warning, Console, "stall ep=0x%02x", ep);

Retrieval
---------

On the next boot ``journal_init()`` dumps the most recent
``CONFIG_JOURNAL_DUMP_LAST_N`` entries to the console, bracketed by
``==== journal ====`` / ``==== end journal ====``. The raw partition is also
readable out-of-band (e.g. ``esptool read-flash`` then ``strings``).

Tunables (Kconfig, under ``CONFIG_JOURNAL``)
--------------------------------------------

``JOURNAL_LINE_MAX`` (96), ``JOURNAL_PEND_SLOTS`` (24), ``JOURNAL_DUMP_LAST_N``
(100), ``JOURNAL_DRAIN_PERIOD_MS`` (2000), ``JOURNAL_DRAIN_MAX_PER_RUN`` (4),
``JOURNAL_FCB_MAGIC`` (0x6a626c67), plus the standard ``JOURNAL_LOG_LEVEL``.
