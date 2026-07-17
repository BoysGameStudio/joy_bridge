/*
 * Keyboard (USB HID boot protocol) -> Pro2 input-layer glue.
 *
 * Implemented by src/input/kbd_map.c (keyboard report -> merge composer).
 * The USBH boot-keyboard class driver (src/usb_host/kbd_class.c) calls
 * kbd_map_report() for every interrupt-IN report and kbd_map_neutral() when
 * the keyboard is removed.
 */

#ifndef INPUT_KBD_MAP_H
#define INPUT_KBD_MAP_H

#include <stddef.h>
#include <stdint.h>

/* Feed one raw boot-protocol keyboard report (8 bytes: modifier, reserved,
 * 6 key slots) into the input layer. */
void kbd_map_report(const uint8_t *report, size_t len);

/* Release every key: parks the keyboard's contribution at neutral so a
 * removed keyboard does not leave buttons stuck pressed on the console. */
void kbd_map_neutral(void);

#endif /* INPUT_KBD_MAP_H */
