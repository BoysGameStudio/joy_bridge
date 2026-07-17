/*
 * USB HID boot-protocol keyboard report parser — pure, host-agnostic.
 *
 * Deliberately free of any Zephyr includes (stdint/stdbool/stddef only) so
 * the parser can be compiled and unit-tested standalone (see
 * tests/kbd_parse/). A USB host class driver feeds 8-byte boot reports into
 * kbd_state_update(); kbd_state_to_output() maps the tracked state onto the
 * Pro2-style button bytes + stick axes via the keymap in kbd_parse.c.
 *
 * Boot report wire format (USB HID spec appendix B.1):
 *   byte 0    modifier bitmap (bit0 LCtrl .. bit7 RGUI)
 *   byte 1    reserved
 *   bytes 2-7 six keycode slots (0x00 = empty)
 * Release is signalled by a keycode being absent from the next report (slot
 * diff). Usages 0x01-0x03 in any slot indicate the phantom/rollover error
 * state, in which case the whole key array is invalid for that report.
 */

#ifndef JOY_BRIDGE_INPUT_KBD_HID_H_
#define JOY_BRIDGE_INPUT_KBD_HID_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KBD_BOOT_REPORT_LEN 8u
#define KBD_NUM_KEY_SLOTS 6u

typedef struct {
    uint8_t mods;
    uint8_t reserved;
    uint8_t keys[KBD_NUM_KEY_SLOTS];
} __attribute__((packed)) kbd_boot_report_t;

_Static_assert(sizeof(kbd_boot_report_t) == KBD_BOOT_REPORT_LEN,
               "boot report must be exactly 8 bytes");

typedef struct {
    uint8_t mods;                 /* modifier bitmap of the latest accepted report  */
    uint8_t held[KBD_NUM_KEY_SLOTS]; /* key slots of the latest non-phantom report   */
    uint8_t prev[KBD_NUM_KEY_SLOTS]; /* key slots of the report before it (slot diff) */
    bool primed;                  /* true once a non-phantom report was accepted     */
} kbd_state_t;

typedef struct {
    uint8_t btn[3];               /* Pro2 button bytes (see btn_bits_pro2_t)  */
    uint16_t lx, ly, rx, ry;      /* 12-bit stick axes, center 0x800          */
} kbd_output_t;

void kbd_state_init(kbd_state_t *st);

/*
 * Fold one boot report into the state. Returns false when the report is
 * rejected: len < 8 (state untouched) or any key slot carries usage 0x01-0x03
 * (phantom/rollover: key slots ignored for this report, mods still applied,
 * held/prev keep their previous values).
 */
bool kbd_state_update(kbd_state_t *st, const uint8_t *report, size_t len);

bool kbd_key_held(const kbd_state_t *st, uint8_t usage);

/* Apply the keymap: sticks centered on release, opposing direction keys on
 * one axis cancel to center, diagonals deflect both axes. */
void kbd_state_to_output(const kbd_state_t *st, kbd_output_t *out);

#ifdef __cplusplus
}
#endif

#endif /* JOY_BRIDGE_INPUT_KBD_HID_H_ */
