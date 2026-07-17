/*
 * XInput -> Pro2 HID report mapping.
 *
 * Translates a 20-byte XInput (Xbox 360 protocol) gamepad report into the
 * Pro2 controller contribution and feeds it to the input merge composer
 * (input_merge.c), the single writer to the HID engine's back buffer. The
 * composer writes the FULL merged button + stick state on every commit, so
 * each buffer is self-consistent regardless of swap history.
 *
 * Button map is physical-position (preserves muscle memory): the Xbox face
 * button in a given position maps to the Switch button in the same position
 * (Xbox A(bottom)->Switch B, B(right)->A, X(left)->Y, Y(top)->X).
 */

#include "input/xinput.h"
#include "input/input_merge.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(jb_usb, CONFIG_JOY_BRIDGE_LOG_LEVEL);
#include "app_log.h"

/*
 * Stick Y inversion. Default OFF: XInput reports +Y up and the NS2 Pro2 report
 * also uses up = high value, so no axis is inverted — matching the verified
 * upstream (ESP32-BLE5-NSController-Emulator `main/src/transport/xinput_host.c`,
 * which notes "NS2 also uses up = high value, so no axis is inverted"). Tunable
 * if a particular pad reads reversed.
 */
#define XINPUT_INVERT_LY 0
#define XINPUT_INVERT_RY 0

/* XInput 16-bit signed axis -> Pro2 12-bit unsigned axis (centre 0x800). */
static inline uint16_t scale_axis(int16_t v, bool invert)
{
    uint16_t s = (uint16_t)(((int32_t)v + 32768) >> 4); /* 0..4095, centre 0x800 */
    if (s > 0x0FFF) {
        s = 0x0FFF;
    }
    return invert ? (uint16_t)(0x0FFF - s) : s;
}

void xinput_map_report(const xinput_report_t *r)
{
    if (r == NULL) {
        return;
    }

#ifdef CONFIG_JOY_BRIDGE_DIAG_LATENCY
    /* Stamp the moment this XInput report arrived (consumed by the report
     * thread to compute arrival->notify latency). */
    extern volatile uint32_t jb_diag_in_cyc;
    extern volatile uint32_t jb_diag_in_cnt;
    jb_diag_in_cyc = k_cycle_get_32();
    jb_diag_in_cnt++;
#endif

    const uint8_t b1 = r->buttons1;
    const uint8_t b2 = r->buttons2;
    const bool lt = r->left_trigger  >= XINPUT_TRIGGER_THRESHOLD;
    const bool rt = r->right_trigger >= XINPUT_TRIGGER_THRESHOLD;

    /* Build the three Pro2 button bytes directly (see btn_bits_pro2_t). */
    input_contrib_t c = {0};

    /* byte 0: B,A,Y,X,R,ZR,Plus,RClick */
    if (b2 & XINPUT_A)     c.btn[0] |= 0x01; /* Xbox A (bottom) -> Switch B */
    if (b2 & XINPUT_B)     c.btn[0] |= 0x02; /* Xbox B (right)  -> Switch A */
    if (b2 & XINPUT_X)     c.btn[0] |= 0x04; /* Xbox X (left)   -> Switch Y */
    if (b2 & XINPUT_Y)     c.btn[0] |= 0x08; /* Xbox Y (top)    -> Switch X */
    if (b2 & XINPUT_RB)    c.btn[0] |= 0x10; /* R  */
    if (rt)                c.btn[0] |= 0x20; /* ZR */
    if (b1 & XINPUT_START) c.btn[0] |= 0x40; /* Plus   */
    if (b1 & XINPUT_R3)    c.btn[0] |= 0x80; /* RClick */

    /* byte 1: Down,Right,Left,Up,L,ZL,Minus,LClick */
    if (b1 & XINPUT_DPAD_DOWN)  c.btn[1] |= 0x01;
    if (b1 & XINPUT_DPAD_RIGHT) c.btn[1] |= 0x02;
    if (b1 & XINPUT_DPAD_LEFT)  c.btn[1] |= 0x04;
    if (b1 & XINPUT_DPAD_UP)    c.btn[1] |= 0x08;
    if (b2 & XINPUT_LB)    c.btn[1] |= 0x10; /* L  */
    if (lt)                c.btn[1] |= 0x20; /* ZL */
    if (b1 & XINPUT_BACK)  c.btn[1] |= 0x40; /* Minus  */
    if (b1 & XINPUT_L3)    c.btn[1] |= 0x80; /* LClick */

    /* byte 2: Home,Capture,GR,GL,C (only Home mapped) */
    if (b2 & XINPUT_GUIDE) c.btn[2] |= 0x01; /* Home */

    c.lx = scale_axis(r->lx, false);
    c.ly = scale_axis(r->ly, XINPUT_INVERT_LY);
    c.rx = scale_axis(r->rx, false);
    c.ry = scale_axis(r->ry, XINPUT_INVERT_RY);

    input_merge_update(INPUT_SRC_XINPUT, &c);
}

void xinput_map_neutral(void)
{
    input_merge_clear(INPUT_SRC_XINPUT);
}
