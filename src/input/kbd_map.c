/*
 * Keyboard (USB HID boot protocol) -> Pro2 input-layer glue.
 *
 * The USBH boot-keyboard class driver (src/usb_host/kbd_class.c) feeds every
 * 8-byte interrupt-IN report to kbd_map_report() and calls kbd_map_neutral()
 * on removal. This mapper folds the report into the parser state
 * (src/input/kbd_parse.c), converts the tracked state to a Pro2 contribution,
 * and publishes it to the input merge composer (src/input/input_merge.c), the
 * single writer to the HID engine's back buffer.
 *
 * A rejected report (short, or phantom/rollover) still re-publishes the
 * current state: phantom reports apply the modifier byte but keep the held
 * keys, so the merged output must reflect that rather than go stale.
 *
 * Built only when CONFIG_JOY_BRIDGE_USB_KBD=y (compiles empty otherwise).
 */

#include <zephyr/kernel.h>

#ifdef CONFIG_JOY_BRIDGE_USB_KBD

#include "input/kbd_map.h"
#include "input/kbd_hid.h"
#include "input/input_merge.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(jb_ctrl, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

static kbd_state_t kbd_state;
static bool kbd_state_ready; /* one-time init guard */

/* Convert the tracked state and publish it. kbd_output_t and input_contrib_t
 * are structurally identical but distinct types — copy field-by-field. */
static void kbd_publish(void)
{
	kbd_output_t out;
	input_contrib_t contrib;

	kbd_state_to_output(&kbd_state, &out);
	contrib.btn[0] = out.btn[0];
	contrib.btn[1] = out.btn[1];
	contrib.btn[2] = out.btn[2];
	contrib.lx = out.lx;
	contrib.ly = out.ly;
	contrib.rx = out.rx;
	contrib.ry = out.ry;
	input_merge_update(INPUT_SRC_KBD, &contrib);
}

void kbd_map_report(const uint8_t *report, size_t len)
{
	if (!kbd_state_ready) {
		kbd_state_init(&kbd_state);
		kbd_state_ready = true;
	}
	(void)kbd_state_update(&kbd_state, report, len);
	kbd_publish();
}

void kbd_map_neutral(void)
{
	kbd_state_init(&kbd_state);
	kbd_state_ready = true;
	input_merge_clear(INPUT_SRC_KBD);
}

#endif /* CONFIG_JOY_BRIDGE_USB_KBD */
